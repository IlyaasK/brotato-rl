# Observation / action parity audit (in-game bridge mod vs shared core)

Scope: the 198-float observation and 31 discrete actions (the original 161-float table plus the v4 Demon/endless extension below), compared field by
field between the in-game observer (`bridge/mod/res/mods-unpacked/rlbridge-headlessrl/rl_obs.gd`,
played through the game by `mod_main.gd`) and the shared CPU/CUDA/Metal core
(`ocean/brotato/brotato_core.h` `bt_core_observe` + `brotato_weapon_observe.h`
`bt_weapon_observe_append`, driven by `bt_core_step`). Ground truth for the game
side is the recovered GDScript plus Godot 3.x engine behaviour; the authoritative
schema text is `bridge/state_contract.json`.

Line references are to the files as of this audit. "Bridge" = GDScript line,
"Sim" = C header line.

The tree is edited concurrently by several agents, so absolute C line numbers can
shift; the cited function/field names are stable identifiers.

## How each claim was verified

* **Slot-by-slot source diff** of `rl_obs.gd`/`mod_main.gd` against
  `bt_core_observe`/`bt_weapon_observe_append`/`bt_core_step`.
* **Historical recorded game trace** `migration/game/rl_trace.contact161_normal.jsonl`
  (480 rows, seed 1, Bull, Danger 5, weaponless) replayed through the real core
  (`tests/brotato_core_trace.cpp`, built with `g++ -std=c++17 -O2 -Wno-narrowing -ffp-contract=off -I.`) and compared
  at atol=rtol=1e-5: **every slot matches to at least tick 278**; the first
  tolerance failure is tick 279 `obs[37]` (an enemy that the game killed and the
  sim did not - enemy-death divergence, not an encoding difference), and the
  first health difference is tick 329. `obs[3]` is byte-identical, `obs[4..5]`
  within 5e-7 (float32 vs float64), `obs[66]` within 6.4e-6.
* **Engine-timing derivations** confirmed against the trace: the tick-0 timer
  (`obs[66]` = 1199/1200 first sample) proves the bridge samples *after* the
  physics tick and the sim's `-BT_DT` compensation is exactly right; the
  invincibility window (`obs[6]`) pins the Godot `Timer` countdown rule.
* Tests: `tests/test_brotato_weapon_observe.cpp` (79 checks) and
  `tests/test_brotato_core.cpp` (252 checks) pass.

## Slot-by-slot table

Sim slot indices are the base of each field inside `bt_core_observe`; weapon
slots 79+8i come from `bt_weapon_observe_append` (called at the end of
`bt_core_observe`).

### Player block 0..10

| Slot | Meaning | Bridge | Sim | Verdict |
|---|---|---|---|---|
| 0 | hp / max_hp | `rl_obs.gd:985` (`hp/max_hp if max_hp>0 else 0`) | `brotato_core.h:1223` | OK (max_hp >= 1 by construction; game guard is defensive only) |
| 1 | max_hp / 64 | `:986` (`*0.015625`) | `:1224` | OK |
| 2 | regen / 30 | `:987`, `Utils.get_stat(stat_hp_regeneration_hash)` `:974` | `:1225` (`s->regen` = gained stat, uncapped, `:2008`) | OK. `get_capped_stat` would `printerr` for regen (utils.gd:469-484 only caps hp/speed/dodge/crit); `state_contract.json` misnames it - bridge code is right |
| 3 | armor / 20 | `:988`, `Utils.get_stat(stat_armor_hash)` `:974` (the STAT, not truncated) | `:1231` via `bt_core_stat_read(s, 2)` | **FIXED** (was the truncated `s->armor` = `(int)stat`, `:2011`). Damage path still uses the truncated `s->armor` (player.gd:353 uses `max_stats.armor`); see discrepancy D3 |
| 4 | px / 2048 | `:989` / `arena_w` | `:1232` / `BT_ARENA_W` | OK (both 2048; `gen_obs_tables.py` emits BT_ARENA_W) |
| 5 | py / 1536 | `:990` / `arena_h` | `:1233` / `BT_ARENA_H` | OK |
| 6 | iframe active | `:991`; `not timer.is_stopped() and time_left > 0` `:979-981` | `:1234` (`s->iframe > 0`) | **ENCODING OK, DURATION DIVERGES** - see D4/D5 (window 2 ticks long, root cause outside `bt_core_observe`) |
| 7 | phase 0/1/2 | `:992`; `mod_main.gd:59-61` PHASE_* 0/1/2 | `:1235` `(float)s->phase` | OK (same 0/1/2 domain; phase-transition tick timing is a residual gap, G3) |
| 8 | materials / 100 | `:993` `RunData.get_player_gold` | `:1236` `s->materials` | OK |
| 9 | level / 20 | `:994` | `:1237` | OK |
| 10 | xp / xp_need | `:995`, `get_next_level_xp_needed(0)` | `:1238`, `bt_xp_needed(level)` `:430` | OK for Bull/Knight/Demon. The game multiplies by `(1+next_level_xp_needed/100)` (run_data.gd:768) - only Baby/Mutant/Technomage produce that key; the sim omits it (gap G1) |

### Shop item block 11..26 (4 slots x 4)

| Slot | Meaning | Bridge | Sim | Verdict |
|---|---|---|---|---|
| 11+4i | item tier / 4 | `rl_obs.gd:509-524` `shop_slot_features`, `:1027` | `:1244` | OK (`BT_ITEMS[0].tier` = 1 for the empty encoding, matching the trace 0.25) |
| 12+4i | materials >= price | same, live `offer.value` as price `:509,516` | `:1245` `bt_shop_price` `:414` | OK for Bull (same item-price rule, item_service get_item_price); a weapon offer keeps index 0 on both sides (bridge `:1015-1019` weapon offer -> `pass`, sim `:1241-1243`). Price equality is shop-domain (verify in AuditShop) |
| 13+4i | dominant stat / 10 | `items[].dom` from `gen_obs_tables.py` (largest \|value\|, first wins) | `:1247-1251` same rule | OK |
| 14+4i | clamp(dominant value * 0.1) | `clamp(mag*0.1,-1,1)` `:523` | `:1252` | OK |

### Level-up block 27..34 (4 slots x 2)

| Slot | Meaning | Bridge | Sim | Verdict |
|---|---|---|---|---|
| 27+2i | upgrade stat / 12 | `rl_obs.gd:890-951` `upgrade_offer_features`, `:1050` | `:1256` | OK (divides by 12 whatever the stat id, so ids 12..18 exceed 1.0 on both sides) |
| 28+2i | clamp(upgrade value * 0.1) | `clamp(best_value*0.1,-1,1)` `:951` | `:1257` | OK (the bridge asserts the live value equals the core BT_UPGRADES row value, else reports unmapped) |

### Enemy block 35..64 (6 slots x 5)

| Slot | Meaning | Bridge | Sim | Verdict |
|---|---|---|---|---|
| 35+5k | (ex-px)/2048 | `rl_obs.gd:403-446` `enemy_features`, `:1059` | `:1277` | OK |
| 36+5k | (ey-py)/1536 | same | `:1278` | OK |
| 37+5k | ehp/emax | same, guarded `max_hp>0 else 0` | `:1279` | OK (emax >= 1 in practice) |
| 38+5k | edmg * 0.1 | same | `:1280` | OK |
| 39+5k | elite 0/1 (`enemy.is_elite`) | `enemy.is_elite` (`rl_obs.gd:488-491`) | `:1995` `(s->eelite[e] == 1 ? 1 : 0)` | OK (fixed — see note) |

*39+5k* was written as the raw 3-valued kind class until this fix; the field is
the source's `Enemy.is_elite` boolean (`ItemEnemy.gd:5`, `boss.gd:4` defaults
true, overridden **false** by every shipped boss scene: `predator.tscn:26`,
`invoker.tscn:24`, `eel.tscn:29`, `dead_whale.tscn:29`) and **true** by every
elite scene (butcher/colossus/croc/gargoyle/mantis/mom/monk/rhino + the 9 DLC
elites). So class 1 (elite) -> 1 and classes 0/2 (enemy/boss) -> 0. The old write
put a 2 into the field for a wave-20 boss (game 0) — the first divergence of a
seeded wave-20 capture at tick 63; the same expression is applied in
`brotato_gpu_observe.cuh:237` to keep the GPU observation bit-identical.

*Ordering*: both accumulate squared distance in source order and selection-sort
the first `min(n,6)` with the same strict `<` tie-break (bridge `:414-430`, sim
`:1270-1276`). The bridge's source order is scene-tree (spawn) order, the sim's
is SoA pool index order: **equal-distance ties may select a different enemy**
(the documented divergence, `state_contract.json.known_divergences[0]`) - not
fixable in the sim.
*Population*: the bridge counts Enemy nodes including a 0-hp node that is still
in the tree for a frame or two after death; the sim's `ealive` drops a dead
enemy. Visible as part of D1.

### Wave block 65..67

| Slot | Meaning | Bridge | Sim | Verdict |
|---|---|---|---|---|
| 65 | wave / 20 | `rl_obs.gd:1071`, `RunData.current_wave` | `:1284` `s->wave` / `BT_WAVES` | OK (both stay on the completed wave through the shop) |
| 66 | time_left / wave_duration | `:1073`, live `WaveTimer.time_left` | `:1286-1290`, `time_left - (phase==0 ? BT_DT : 0)` | OK. Verified: trace tick 0 = 1199/1200 on both; the `-BT_DT` reproduces the bridge sampling after the physics tick; level-up/shop timers are paused and not decremented (`:1287`) |
| 67 | enemy count / 100 | `:1075` `min(nodes, max_enemies) * (1/max_enemies)` — `max_enemies` comes from the tables (`:261`), so after `BT_MAX` grew 100 -> 192 (the endless pool bound) it became **1/192** | `brotato_core.h:1967` `n * BT_ENEMY_COUNT_SCALE` with `brotato_state.h:252` `(1.0f/100.0f)` = the AUTHORED cap, independent of the pool bound | **DRIFT introduced by the BT_MAX growth** (found in Main's verified capture: tick 123 ref 1/192 vs sim 1/100). Fix is the bridge side (divide by the authored cap 100, keep `max_enemies` only for the `min` collection cap and report the real denominator in `scale`); taken by Main, not by this audit. Population difference is D1 |

### Enemy-projectile block 68..76 (3 slots x 3)

| Slot | Meaning | Bridge | Sim | Verdict |
|---|---|---|---|---|
| 68+3k | (dx)/2048 | `rl_obs.gd:448-478` `projectile_features`, `:1087` | `:1315` | OK |
| 69+3k | (dy)/1536 | same | `:1316` | OK |
| 70+3k | get_damage() * 0.1 | same, `node.get_damage()` | `:1317` `s->prdmg` | OK |

*Selection*: both take the 3 nearest by squared distance and consume each chosen
entry so a projectile fills at most one slot; empty slots are 0.0 on both sides.
Three sim bugs against the game were found and fixed:

* **D2** cutoff was `262144.0f` (512^2, `brotato_core.h:1314` before) vs the
  bridge's `PROJ_MAX_D2 = 70000.0` (`rl_obs.gd:70`, documented in
  `state_contract.json` obs[68..76]). A projectile at 265..512 px was reported
  by the sim and not by the game. Fixed to a named `BT_OBS_PROJ_MAX_D2` =
  70000.0 (`brotato_core.h:1208-1210`).
* **D2b** the "already consumed" marker `pralive[best] = 2` was not skipped by
  the selection test (`if (!s->pralive[i]) continue;` treats 2 as alive), so the
  same nearest projectile could fill all three slots. Fixed to
  `if (s->pralive[i] != 1) continue;` (`:1299`).
* **D2c** the pool mixes enemy shots and player-owned shots; the bridge reads
  only the arena's `EnemyProjectiles` container (`shooting_attack_behavior.gd:203`
  -> `main.add_enemy_projectile`), while pet/turret and item special shots go to
  `_player_projectiles` (`weapon_service.gd:392`). Friendly pool entries with no
  source enemy (`prfriendly && prsrc == 0xFFFF`) are now skipped (`:1309`); a
  charmed enemy's shot is friendly but keeps its `prsrc` and is still parented
  under `EnemyProjectiles`, so it stays in.
* Tie-break: both pick the lowest source index on equal distance (bridge scene
  order, sim pool order) - same documented divergence as enemies.

### Consumable block 77..78

| Slot | Meaning | Bridge | Sim | Verdict |
|---|---|---|---|---|
| 77 | nearest consumable dx / 2048 | `rl_obs.gd:480-506`, `:1099` | `:1332` | OK |
| 78 | nearest consumable dy / 1536 | same | `:1333` | OK |

Single nearest (no cutoff), 0.0 when none, on both sides.

### Weapon block 79..126 (6 slots x 8) - `brotato_weapon_observe.h`

| Slot | Meaning | Bridge | Sim | Verdict |
|---|---|---|---|---|
| 79+8i | id (index+1)/(N+1), 0.0 empty | `rl_obs.gd:576-618` `weapon_live_features` / `:694-722` `weapon_rundata_features` | `:145` | OK (0xFFFF is the only empty sentinel; BT_WEAPONS row 0 emits 1/(N+1) != 0) |
| 80+8i | tier / 4 | `rl_obs.gd:603` table row tier | `:146` `wpn_tier` = `bt_weapon_tier(id)` | OK (the inventory stores the row tier) |
| 81+8i | kind 0/1 | `:604` table row kind | `:147` `def->kind` | OK |
| 82+8i | clamp(cooldown_left/cooldown,0,1) ticks | `:594-597` `_current_cooldown` / `current_stats.cooldown` | `:138-143` `st.cooldown_left`/`st.cooldown` (both ticks) | OK; **always 0.0 in phase 1** on both sides (bridge shop path, sim `:143`) and the stored counter is not reset |
| 83+8i | damage * 0.1 | live `current_stats.damage` | `:149` `bt_weapon_stats` | OK (weapon-stats domain) |
| 84+8i | max_range / 2048 | live `max_range` | `:150` | OK |
| 85+8i | crit_chance (0..1 frac) | live `crit_chance` | `:151` | OK |
| 86+8i | cursed 0/1 | `is_cursed` | `:152` `wpn_cursed` | OK |

Empty slot emits eight 0.0 on both sides. The bridge reads the live
`current_weapons` nodes in combat/level-up and the durable inventory
(`RunData.get_player_weapons(0)`) in the shop with cooldown 0.0; the sim reads
its slot state, so a buy/recycle/combine is visible in the next row on both.

### Typed shop offers 127..138 (4 x 3)

| Slot | Meaning | Bridge | Sim | Verdict |
|---|---|---|---|---|
| 127+3i | type 0/1/2 | `rl_obs.gd:725-758` `offer_block` | `:158` `shop_kind` | OK (NONE/ITEM/WEAPON = 0/1/2); `shop_kind` is cleared on skip (`brotato_progression.h` action 8) and on reset, so combat/level-up rows emit 0 like the bridge |
| 128+3i | (index+1)/(count+1) | item -> BT_ITEMS, weapon -> BT_WEAPONS | `:160-164` | OK |
| 129+3i | cursed 0/1 | live `is_cursed` | `:166` `shop_cursed` | OK |

### Profile 139..140

| Slot | Meaning | Bridge | Sim | Verdict |
|---|---|---|---|---|
| 139 | (character+1)/(N+1) | `rl_obs.gd:760-791` `profile_block` | `:170` | OK (Bull 0 -> 0.25, as in the trace) |
| 140 | weapon_slot_limit / 6 | live `RunData.get_player_effect(weapon_slot_hash)` | `:171` `weapon_slot_limit` / `BT_CHARACTER_SLOT_DEFAULT` | OK (Bull 0 -> 0.0) |

### Actionable shop state 141..160

| Slot | Meaning | Bridge | Sim | Verdict |
|---|---|---|---|---|
| 141 | currency is hp (Demon) | `rl_obs.gd:793-840`, `:834` | `:200` | OK (both 0 outside phase 1) |
| 142+4i | exact price | live `ShopItem.value` | `:218` `bt_wshop_price_units` | OK by contract; shop-domain (verify in AuditShop) |
| 143+4i | eligibility | weapon `container._can_weapon_be_bought`, item always 1 | `:207-216` `bt_wshop_can_buy` | OK (documented: lock_current_weapons unreachable for Bull/Knight/Demon) |
| 144+4i | affordable | `currency >= price` | `:217` `bt_wshop_currency >= price` | OK |
| 145+4i | buyable | eligibility && affordable && `not _is_delay_active` | `:221` `shop_timer <= 0` | OK |
| 158-160 | reroll price / free / available | `rl_obs.gd:856-888` `reroll_features` (gold always) | `:231-239` | OK; both 0 outside phase 1 |

## Action table (21 discrete actions)

| Action | Bridge/game side | Sim side | Verdict |
|---|---|---|---|
| 0 HOLD | presses nothing (`rl_inject.gd:169-187`); `Input.get_vector` -> (0,0) | `BT_MOVE_X[0]=BT_MOVE_Y[0]=0` (`brotato_core.h:111-112`) | OK; movement (0,0) |
| 1..8 N,NE,E,SE,S,SW,W,NW | MOVE_X/MOVE_Y tables (`rl_inject.gd:26-29`), pressed as `analog_move_left/right/up/down` (`:41`) and normalised by the engine's `Input.get_vector` (`:116`; default deadzone 0.5, full-strength presses), read by `PlayerMovementBehavior.get_vector_for_input_prefix("analog_")` (player_movement_behavior.gd:19,29-33) | identical table, literal diagonals `0.70710678f` (`:111-112`); velocity = vector * speed, then `test_motion` (`:3779-3800`) | OK. Trace tick 1 (action 8 NW) position matches to <5e-7; `action_applied` in the game is `_current_movement == Input.get_vector(...)` |
| 9..14 recycle weapon slot (a-9) | shop phase only; combat records HOLD (`state_contract.json.action.not_implemented`) | `bt_wshop_owned_action` -> `bt_wshop_recycle(a-9)` (`brotato_weapon_shop.h`), reached from `bt_progress_shop_action` (`brotato_progression.h:485,562`); combat: `BT_MOVE_X[9..20]` are implicit 0 in the 21-entry array -> HOLD | OK |
| 15..20 combine weapon slot (a-15) | same | `bt_wshop_combine(a-15)` | OK |
| level-up 0..3 | choose that offer through the UpgradesUI container (`mod_main.gd:_drive_level_up`) | `bt_progress_choose_upgrade(s, action)` (`brotato_progression.h:283-297`) | OK |
| shop 0..3 buy, 4 reroll, 8 skip | game's own handlers (`rl_progress.shop_action`) | `bt_progress_shop_action` (`brotato_progression.h:485`) 0..3 buy, 4 reroll, 8 skip | OK; 5..7 are no-ops on both sides |
| action repeat / interest | none: the bridge releases all four analog actions and presses the current one every tick (`rl_inject.gd:169-176`); no latching | none: `bt_core_step` reads `BT_MOVE_X[action]` for the step being produced | OK |
| frame-skip | 1 physics tick = 1 recorded row (`mod_main.gd:447-477`, SAMPLE_EVERY=1 by default) | 1 `bt_core_step` = 1 tick, `BT_DT = 1/60` (`brotato_state.h:216`) | OK |
| which frame an action applies on | row `t` = (obs post-step `t`, action injected for step `t`); the bridge callback runs after the tick's physics, sets the input for the next tick, and `verify()` reports the action of the tick just completed (`mod_main.gd:296-304,445-477`) | `bt_core_step(action)` applies the action inside the step it observes | OK: `replay_cpu.py`/`brotato_core_trace.cpp` feed `action[t]` then compare `obs[t]` and match to tick 278 |
| out-of-range | `direction_for` returns (0,0) for <0 or >20; the recording only emits 0..20 | clamps to [0,20] (`brotato_core.h:3746-3747`) | OK |

## Discrepancies found

| id | System | Game side (file:line) | Sim side (file:line) | Impact on transfer | Fixed? |
|---|---|---|---|---|---|
| D2 | obs[68..76] projectile cutoff | `rl_obs.gd:70` `PROJ_MAX_D2 = 70000.0`; `state_contract.json` obs[68..76] | `brotato_core.h:1314` was `262144.0f` (512^2) | a projectile at 265..512 px appears in 3 sim slots and in none in the game: policy sees obstacles that do not exist at deployment | YES - `BT_OBS_PROJ_MAX_D2 70000.0f` (`brotato_core.h:1208-1210`) |
| D2b | obs[68..76] distinctness | `rl_obs.gd:448-478` consumes each chosen node | `brotato_core.h:1299` (was `!s->pralive[i]`, so the consumed marker 2 was treated as alive) | with few projectiles the sim repeated the nearest one in up to 3 slots (3x the real information, wrong slots) | YES - `!= 1` skip |
| D2c | obs[68..76] population | reads the `EnemyProjectiles` container only (`shooting_attack_behavior.gd:203`, `main.tscn:250`) | `brotato_core.h:1309` (added) skips `prfriendly && prsrc == 0xFFFF` | pet/turret/item shots were reported as enemy projectiles (Knight/Demon/pet builds) | YES |
| D3 | obs[3] armor | `rl_obs.gd:974,988` `Utils.get_stat(stat_armor_hash)` (fractional stat) | `brotato_core.h:1231` now `bt_core_stat_read(s,2)`; was the truncated `s->armor` (`:2011`) | only when the gained armor stat is fractional (a `stat_gain` modifier); latent for the recorded Bull run (armor 10 -> 0.5 on both sides) | YES (damage path unchanged: player.gd:353 uses the truncated `max_stats.armor`, sim `bt_armor_mult(s->armor)`) |
| D4 | obs[6] invincibility window | Godot `Timer` (`player.gd:425-438,565-575`): `time_left` is a double accumulating the float32 physics delta, so `0.2 - 12*(1/60)` = -1.04e-8 and the window is 11 observed ticks (trace ticks 278..288, stopped 289) | `brotato_core.h:3957` decrements a float32 `s->iframe` and the contact damage at `:4636` runs *after* it, so the damage tick is not decremented and the 12th float32 subtraction leaves +1.1e-8: window 13 ticks (trace 278..290) | the policy is invulnerable/moves freely 2 ticks longer than in the game, and `bt_player_damage`'s `iframe > 0` gate blocks a contact one tick longer | NO - root cause in the step/contact timing, not in `bt_core_observe`; reported to the replay-divergence owner (ReplayDivergence) with the numeric evidence |
| D1 | enemy block 35..67 population | the game keeps a 0-hp `Enemy` node in the tree for a frame or two after death, so `obs[37]` reads 0 and the node keeps its slot (trace ticks 279..288) | the sim drops `ealive` on death and reports a different, alive enemy (`obs[36]` jumps at 280) | an enemy's death/retaliation timing differs (game kills the contact enemy at 279, sim does not): state divergence, already covered by the replay-divergence work | NO - enemy lifecycle/contact damage, not the encoder |

### D4 detail for the replay-divergence owner

* `s->iframe` (`brotato_state.h`) is float32; `0.2f` minus twelve `BT_DT`
  subtractions ends at `+1.1175871e-08`, so `iframe > 0` is still true on the
  12th tick. Godot's `Timer::time_left` is a double accumulated from the float32
  `get_physics_process_delta_time()`, which lands at `-1.043e-08` and stops.
* The contact damage call (`brotato_core.h:4636`) is ~680 lines after the iframe
  decrement (`:3957`), so the tick that applies the damage does not decrement;
  the game's Timer processes its physics notification after the damage in the
  same tick.
* Both `bt_core_observe:1234` and `bt_player_damage:3752` read the same flag, so
  a fix must be at the update site (count whole ticks, or compare against a
  tolerance / use a double), not in the observer.

## Bridge-side changes needed (read-only files: report only)

| id | File:line | Change | Why |
|---|---|---|---|
| B1 | `rl_obs.gd:70` `PROJ_MAX_D2`, `gen_obs_tables.py:175-186` `wanted` | add the cutoff to the emitted `core_revision.obs_constants` (from a core `#define BT_OBS_PROJ_MAX_D2`, which now exists in `brotato_core.h:1208`) and read it in `_apply_constants` (`rl_obs.gd:220-228`) | the cutoff is the only obs constant still hard-coded on both sides with no revision check; it already drifted once (D2) |
| B2 | `gen_obs_tables.py:175-186` | `BT_SLOTS`/`BT_MAX_PROJ`/`BT_MAX_CONS`/`BT_ACTIONS` are requested but `rl_obs.gd` never consumes them (SLOTS/PROJ_SLOTS are hard-coded at `:67-69`); wire them or drop them from `wanted` | a table that does not feed the observer is a silent drift risk |
| B3 | `state_contract.json` obs[2], obs[3] | text says `Utils.get_capped_stat(stat_hp_regeneration_hash/stat_armor_hash)` | `get_capped_stat` errors for a stat with no `*_cap` (utils.gd:469-484); the bridge correctly uses `Utils.get_stat`. Doc-only, no code change |
| B4 | `state_contract.json.action.applied_semantics` vs `mod_main.gd:486-506,509-530` | the shop/level-up rows record the observation *before* applying the action while combat rows record it *after* the step that produced it; the prose only describes the combat case | a replay consumer pairing `(obs[t], action[t])` must know shop rows use the pre-action convention; document it (or sample post-action in the shop) |
| B5 | `mod_main.gd:324-330` `_phase()` | the level-up phase is read from `UpgradesUI.visible` in the arena scene, while the sim sets `phase = 2` inside the tick that ends the wave | one-tick phase-boundary difference at wave end; confirm the game's UI becomes visible on the same tick the wave timer expires |

## Residual gaps / unverifiable here

* **G1** `obs[10]`: the sim omits the `next_level_xp_needed` multiplier
  (run_data.gd:768). Unreachable for Bull/Knight/Demon (only Baby/Mutant/
  Technomage key it), but it must be added before training those characters.
* **G2** `obs[6]` duration (D4) and `obs[35..64]` population (D1) as above.
* **G3** phase-boundary tick (B5) could not be checked against a trace: the only
  recorded trace (480 rows) is combat-only within wave 1.
* **G4** shop/level-up blocks (11..34, 127..160) have no recorded non-combat
  trace in the repo, so they were verified by source diff only; the earlier
  shop smoke run used invulnerability and is not a parity proof.
* **G8 (resolved: a comparison-config issue, not a sim defect)**: the `obs[82]` divergence on the pinned capture was the replay running the default `particles_rng = 1` while the capture had been recorded with the bridge's particle suppression (`emitting = false`). Replaying with `particles_rng = 0` (argv[9] = 0) removes exactly the sim's player-fx draws - 90 by tick 157, matching the +88 decoded from the rolled cooldown (game 46.8073 = stream index 267 vs the sim's 51.43 = index 355) - and leaves a ~2-draw residual (a ~1.9-frame difference in the rolled cooldown = 2 draws of the 1-draw/frame spawner shuffle), with no change needed on either side. The sim's player-fx model itself is correct (it matches the game exactly when the game's particles are live, as the Bull pair showed), and **fixed-tick comparisons and mismatch COUNTS are not valid arbiters** here: the rolled cooldown moves the next shot's timing (with `particles_rng = 0` the sim's first shot moves 157 -> 156, which reshuffles the cull/death draws), so only timing-free measurements (value -> index decode, or event-aligned comparison) may be used. This is the third instance of the same class as `wave_duration` and `audio_rng`: **a knob the trace does not record** (this capture's rows/meta carry neither `audio_rng` nor `particles_rng`, nor `scale.enemy_count_denominator`), so a replay cannot auto-select them - the capture-producer item logged for BridgeGaps/Main.
* **G9 (out of area, owner = items/effects)**: the `minimum_weapon_cooldowns` / `maximum_weapon_cooldowns` item family is inert in the sim - `bt_wstats_min_cd_add` / `bt_wstats_max_cd_add` (brotato_weapon_stats.h:536/552) have no callers and `bt_apply_item` reads no `WEAPON_COOLDOWN` (opcode 33), so `s->min_wcd[]`/`max_wcd[]` are only ever zeroed; the consumer half (`bt_ws_apply_min_cd`/`max_cd`, weapon_service.gd:558-600) is landed and waiting. A Demon build carrying such an item would keep the base cooldown in the sim. Routed by AuditRanged.
* **Ledger correction**: the "+3 draws per enemy birth" item is a MYTH - the residual was one audio playback pitch draw and with `audio_rng = 0` the sim matches the game counter exactly (563 = 563 at tick 239). Dropped.
* **G7 (fixed by Main, recorded here)**: `obs[67]` was normalised by the table-derived `max_enemies`, which the endless work raised from 100 to 192, while the core deliberately keeps the authored `1/100` scale (`brotato_state.h:252`). One policy-visible field disagreed; the fix keeps the authored denominator and must also make the run meta's `scale.max_enemies` report what the field actually uses.
* **G5b (proven inherent)** container order: the game re-parents a pooled node on respawn (main.gd:1649-1660 `get_node_from_pool` -> `add_child`, which moves it to the END of the container) while the sim keeps a stable per-round uid, so two candidates at exactly equal distance can pick a different slot. Closing it exactly needs a per-enemy "container position" bumped on every acquire (brotato_enemy_pool.h) that bt_core_observe can then use as a tie-break; the encoder alone cannot recover it. Handed to the enemy-lifecycle owner.
* **G5** projectile/consumable tie-breaks and enemy ties follow different source
  orders (scene tree vs SoA pool); only observable when two candidates are
  exactly equidistant.
* **G6** `obs[27..34]` offer identity: the bridge maps a live upgrade by
  (stat, tier) and asserts the value equals the core row; the sim uses the row
  index. Correct as long as those rows are unique in `(stat, tier)`, which
  `rl_obs.gd:_index_contract` enforces at load.

# Demon ENDLESS contract (v4) - supersedes the Bull-only scope above

STATUS: the sim + the bridge observer are LANDED (obs 198, actions 31, mask wired on
CPU). The shop-owned slots 165..173, the new action mechanics, endless waves and the
GPU action mask are other owners (section 4).

Target: Demon, Danger 5, endless (wave 21+), executing the ArosRising
High-Endless plan (Ghost Scepter start -> HP-shop + economy -> Pruner / Potato
Thrower / Excalibur -> Spicy Sauce build). Everything below is Demon/endless
specific; the slot table above stays valid for the shared 0..160 fields.

## 0. BLOCKER: the sim has no endless mode

| Fact | Evidence |
|---|---|
| The core ends the run at wave 20 | `brotato_core.h:4707-4714` (`if (s->wave == BT_WAVES) { cleared_game = won = done = 1 }`) |
| Wave data is capped at 20 | `brotato_waves.h:20` `#define BT_WAVES_MAX 20`; `bt_wave_duration` clamps `i >= BT_WAVES_MAX` (`brotato_core.h:403-410`); `bt_build_live_groups` clamps `wi` the same way (`:1000`) |
| The game continues past 20 | `run_data.gd:1645-1646 should_show_endless_button` (wave 19), `:197 is_endless_run`, `:785 get_additional_elites_endless`; `zones/wave_manager.gd:80-96` (`current_wave > nb_of_waves` -> endless elite group + `RunData.get_bosses_to_spawn(true)`) |
| Loot/enemy scaling for endless exists but is unused | `brotato_loot.h:116-122 bt_loot_endless_factor` (called from `:187`, curse `:269`); `brotato_enemy_modifiers.h:32-45` says endless factor == 1.0 (campaign default) |

Without wave 21+ this environment cannot train the target at all: no endless
waves, no endless elites/bosses, no endless shop bans
(`brotato_items.h:1170 banned_items_for_endless`: piggy bank, crown), no endless
enemy scaling. Owner: whoever owns `brotato_waves.h` + the wave-end branch of
`bt_core_step` (NOT this audit's files) - requested from Main.

Also missing in the sim for the target, each a small game-sourced fix:
* `bt_iframes` (`brotato_core.h:174-182`) ignores the endless factor; the game
  divides MIN/MAX_IFRAMES by `max(1.0, get_endless_factor())`
  (`player.gd:565-575`), so obs[6] (and the damage gate) diverge increasingly
  once the factor >= 1.
* The bridge's headless run must select endless when the game offers it
  (`run_data.gd:1645-1646`); the launcher/mod must press that UI once.

## 1. What the real game actually lets a single player do

Audited against the recovered UI (all references are game sources):

| Capability | Where | Phase | In the 21 actions today |
|---|---|---|---|
| move 8-way / hold | `player_movement_behavior.gd:19-33` | combat | yes (0..8) |
| buy offer i | `base_shop.gd:601-615`, `shop_items_container.gd:58-135` | shop | yes (0..3) |
| reroll shop | `base_shop.gd:400-410,479-483` | shop | yes (4) |
| go to next wave | `base_shop.gd:339-349` | shop | yes (8) |
| **lock / unlock offer i** | `shop_item.gd:238-252 change_lock_status` -> `run_data.gd:461-468`; persists via `base_shop.gd:360 fill_shop_items(player_locked_items,...)` + `_update_visual_locks :277-289`; blocked by `disable_item_locking` `:239` | shop | **NO** |
| **recycle (discard) an owned weapon** | `base_shop.gd:706-745 _on_item_discard_button_pressed` (gold = `ItemService.get_recycling_value(current_wave, value, ...)`, coupon delta, `update_recycling_tracking_value`, then `reload_shop_items`) | shop | partly: actions 9..14 exist in the sim bridge contract, but as "recycle" without the gold/recycling-tracking/coupon/reload side effects unless the shop owner implements them |
| **combine weapons** | `base_shop.gd:622-704`: buying a duplicate **when no slot is free auto-combines** (`buy_weapon :601-615` -> `_combine_weapon(...,false)` removes the bought copy + one owned copy of the same `my_id`), or the popup `item_combine_button_pressed`; `is_upgrade` removes only 1 | shop | partly: actions 15..20 exist in the bridge contract; verify the sim's combine matches `_combine_weapon` (nb 2, same `my_id`, tracked value, reroll of the shop) |
| **ban offer i** | `upgrades_ui_player_container.gd:23,113-115` (`BAN_MAX_TOKEN = 8`, `run_data.gd:19,560` -> every player starts with 8), gives gold via `get_recycling_value` and removes the item from the pool | shop | **NO** |
| choose level-up offer i | `upgrades_ui_player_container.gd:203-209` | level-up | yes (0..3) |
| **reroll level-up offers** | `upgrades_ui_player_container.gd:16,57-75,175-199` (`_reroll_price`) | level-up | **NO** |
| **take the crate (item box) item** | `upgrades_ui.gd:99-115 show_consumable_data`; `upgrades_ui_player_container.gd:212-218` | level-up/crate | **NO** (sim auto-takes in `bt_progress_pending_boxes`) |
| **recycle the crate item** | `upgrades_ui_player_container.gd:221-227` -> `upgrades_ui.gd:210-213` -> `main.gd:1145 on_item_box_discard_button_pressed` | level-up/crate | **NO** |
| owned-item recycle (Recycling Machine) | end-of-wave automatic (`run_data.gd:~1640`), not an action | - | n/a |

## Mask parity: artifact conditionals for EVERY action id

`bt_action_legal` (brotato_core.h) is the single source of truth; the CPU writer, the
lane-strided GPU writer (`brotato_gpu_observe.cuh`, now delegating) and the artifact's
in-game mask must agree for every state. Per id, the sim's condition and how the artifact
can express it from the observation:

| ids | sim condition (`bt_action_legal`) | expressible from obs? | artifact expression |
|---|---|---|---|
| 0..3 buy i | live buyable affordable offer and `shop_timer <= 0` | YES - `obs[145+4i]` IS eligibility∧affordable∧delay (`buyable`) | `{"id_base":0,"count":4,"field_base":145,"field_stride":4,"condition":"nonzero"}` |
| 4 reroll | free or affordable and timer expired | YES - `obs[160]` (`reroll available`) | `{"id_base":4,"count":1,"field_base":160,"field_stride":0,"condition":"nonzero"}` |
| 8 skip | always | constant | `legal_by_phase["1_shop"]` |
| 9..14 recycle i | weapon present in slot i | YES - `obs[79+8i] != 0` | `{"id_base":9,"count":6,"field_base":79,"field_stride":8,"condition":"nonzero"}` |
| 15..20 combine i | a second copy of the same variant exists | YES - two slots equal on `obs[79+8i]` | `{"id_base":15,"count":6,"condition":"duplicate_in_block","field_base":79,"field_stride":8}` (a named predicate both consumers implement; no schema change) |
| 21..24 lock i | slot i has a live offer | YES - `obs[127+3i] != 0` | `{"id_base":21,"count":4,"field_base":127,"field_stride":3,"condition":"nonzero"}` |
| 25..28 ban i | ITEM offer **and** `ban_mode && ban_tokens > 0` | YES (v5) - `obs[127+3i] == 1` is the item half; `obs[198] = ban_tokens/8` (0 when ban_mode off) is the token half | `{"clause":"1_shop","id_base":25,"count":4,"tests":[{"op":"equals","value":1,"field_base":127,"field_stride":3},{"op":"nonzero","field_base":198,"field_stride":0}]}` |
| 29/30 crate | an open box offer | YES - `obs[169] != 0` | `legal_by_phase["2_crate"]` (already selected by the crate selector) |
| 0..3 level-up i | offer i present (`lvl_offers[i] != 0`) | YES (v5) - the sim now stores `lvl_offers[i] = BT_UPGRADES row + 1` (0 = absent) and `obs[199+i]` is the unambiguous presence flag | `{"clause":"2_level_up","id_base":0,"count":4,"tests":[{"op":"nonzero","field_base":199,"field_stride":1}]}` |
| 4 level-up reroll | >1 offer and gold >= price | YES (v5) - at least two `obs[199..202] != 0` and `obs[8]*100 >= obs[173]` (compared as `obs[8] >= obs[173]*0.01`, exact) | `{"clause":"2_level_up","id_base":4,"count":1,"tests":[{"op":"block_count_gte","value":2,"field_base":199,"field_stride":1,"count":4},{"op":"ge","field":8,"other":173,"scale":0.01}]}` |

End-to-end check (Main's requirement): the replay output now carries the sim's own row
(`"mask":[31 ints]` from `bt_action_mask` in `tests/brotato_core_trace.cpp`), and the trace
rows must carry the bridge's applied mask per row (BridgeGaps' driver + `rl_trace.gd`);
the check asserts equality on every row of a v5 capture.  Both former exceptions (ban
tokens, level-up presence) are CLOSED by the v5 bump (obs 198 -> 203); the only remaining
residual is bt_weapon_can_combine's upgrade path (a combine whose duplicate exists but is
maxed / has no upgrade path reads legal in the artifact while the sim no-ops it), which the
eval harness counts outside the combine block.  LANDED: `conditionals` are now
clause-scoped `{clause,id_base,count,tests[]}` entries (each test AND-combined), consumed
identically by rl_policy.gd, eval_game.py and the policy exporter.

## 1b. v5 bump (LANDED): 198 -> 203, mask parity closed

The schema bump this section promised landed as obs_version 5 (203 floats):
`obs[198]` = ban tokens / `BT_BAN_MAX_TOKEN` (0 when ban mode is off) and
`obs[199..202]` = per-slot level-up offer presence.  Sim side: `lvl_offers[]`
now stores `BT_UPGRADES row + 1` (0 = absent) via `bt_lvl_offer_row` /
`bt_lvl_offer_present` (brotato_state.h), written/decoded by `bt_core_observe`,
the GPU mirror and `bt_progress_roll_upgrade`/`bt_progress_choose_upgrade`;
`BT_UPGRADE_NONE` is gone.  Bridge side: rl_obs.gd emits the five values live,
rl_trace.gd/parse_trace.py/state_contract.json are `full_v5` / obs_version 5 and
REJECT v4 (198-wide) rows.  Artifact: export_policy.py emits the two remaining
conditionals (ban tokens, level-up presence) plus the level-up reroll, in the
clause-scoped `{clause,id_base,count,tests[]}` form; `unobservable_conditions`
now names only bt_weapon_can_combine's upgrade path.

## 2. Proposed action space (v4)

One discrete head, phase-factored, masked. `src/pufferenv.h:29 Agent.action_mask`
exists and the learner honours it (`src/pufferl.cu:601-672` masks logits,
`src/algo.cu:1411-1491`); the env currently sets it to NULL
(`brotato.h:443`, `brotato.cu:147`). Masked ids must also be ignored by the core
(the current `bt_core_step` only clamps `action` to `0..BT_ACTIONS-1`).

```
BT_ACTIONS = 31
id   semantics                                  phase      notes
0    hold                                       0, and a no-op elsewhere
1..8 N,NE,E,SE,S,SW,W,NW                        0
0..3 buy offer i                                1          (reuses 0..3)
4    reroll                                     1          gold/free reroll
8    go to next wave                           1
9..14   recycle owned weapon slot (id-9)        1          game: discard, +gold
15..20  combine owned weapon slot (id-15)       1
21..24  toggle lock on offer (id-21)            1          NEW
25..28  ban offer (id-25)                       1          NEW (8 tokens)
0..3  choose level-up offer i                   2          (reuses 0..3)
4     reroll level-up offers                    2          NEW
29    take the crate item                       2 (crate)  NEW
30    recycle the crate item                     2 (crate)  NEW
```
Rationale for keeping the old ids: 0..20 keep their meaning, so the combat
contract and every recorded trace stay replayable; new capabilities are appended
and phase-masked. Board mask: combat -> {0..8}; shop -> {0..4,8..28}; level-up
without crate -> {0..4}; crate -> {29,30}.

## 3. Obs additions (v4, +37 floats: 161 -> 198; see 1b for the v5 tail to 203)

Appended after 160 so 0..160 keep their indices and the v3 policy pair stays
comparable field-by-field:

| Slots | Meaning | Sim source | Needs new state? |
|---|---|---|---|
| 161 | harvesting / 100 | `s->harvest` | LANDED (`brotato_core.h` v4 block) |
| 162 | luck / 100 | `s->luck` | LANDED |
| 163 | endless factor, squashed `ef/(1+ef)` (0 through wave 20) | `bt_loot_endless_factor(s->wave)` | LANDED |
| 164 | wave / 50 (endless wave scale; slot 65 keeps wave/20) | `s->wave` | LANDED |
| 165..168 | shop offer lock flag per offer (1/0) | `shop_locked[4]` (AuditShop landed; rank+1, 0 = unlocked) | LANDED |
| 169..172 | crate offer: kind (0/1/2), id (idx+1)/(N+1), cursed, ready | `crate_kind`/`crate_id`/`crate_cursed` (AuditShop; ready = kind != NONE, item boxes are always items) | LANDED |
| 173 | level-up reroll price in gold (0 = free/not offered) | `lvl_reroll_price` (AuditShop; upgrades_ui_player_container.gd:57-75) | LANDED |
| 174..197 | owned copy count of 24 curated guide items, `min(count,4)/4` | `s->item_owned[BT_OBS_GUIDE_ITEMS[i]]` | LANDED (24 ids verified to exist in BT_ITEM_IDX_*) |

Curated guide items for 174..197, in this exact order (the bridge mirrors the same
list as `GUIDE_ITEM_IDS` in `rl_obs.gd`):
bag, crown, hunting
trophy, coupon, dangerous bunny, recycling machine, lure, anvil, alien eyes,
garden, improved tools, explosive shells, plastic explosive, alien tongue, sad
tomato, torture, padding, tardigrade, silver bullet, grind's magical leaf,
vigilante ring, bloody hand, ghost scepter (as an item? no - weapons are already
in 79..126), + reserve. AuditShop/PolicyHarness to confirm the final 24 ids.

Weapon parity note for the plan's weapons: Ghost Scepter (RANGED, 4 tiers,
`brotato_weapons.h:2427-2446`, ids 118-121), Pruner (MELEE, `:2647-2661+`),
Potato Thrower (RANGED, tiers 2-4, `:2547-2562`), Excalibur and Thief Dagger to
be confirmed present in BT_WEAPONS by the same test. Their observed fields
(79..126) follow the verified rules above; combine/recycle must reproduce
`_combine_weapon` and `_on_item_discard_button_pressed` (AuditShop).

## 5. Residual gaps measured after landing

* **obs[7] phase timing**: LinuxGame measured (BROTATO_RL_INSTANT_WAVES=1, Bull,
  Danger 5) that the game's phase does NOT flip on the wave-timer tick: the
  timer hit 0 at tick 485 while still phase 0 / scene "Main", the first phase-1
  shop row was tick 540 (~54 ticks later), and shop->arena flips one tick after
  the Go action. The sim flips `phase` inside the wave-ending tick, so obs[7]
  diverges for that transition window. A replay consumer must key on the
  recorded `phase` (never on a tick count), and the phase model itself is a
  progression-side fix.
* **env[0] sends/closes**: if the arena scene becomes null (death/teardown) the
  bridge counts 300 out-of-scene ticks and ends with reason "unexpected_scene",
  writing no rows during that window.
* **GPU mask**: see CudaParity note above; until then GPU rollouts see an
  all-ones mask (illegal ids are no-ops, not errors).

## 4. Cross-owner dependencies

| Owner | Ask |
|---|---|
| wave/endless owner (Main to assign) | waves 21+ in `brotato_waves.h`/`bt_enter_wave`/wave-end branch; endless elites+bosses (`wave_manager.gd:80-96`); endless enemy scaling (`brotato_enemy_modifiers.h` factor); endless shop bans (`brotato_items.h:1170`); `bt_iframes` endless divisor |
| AuditShop | lock/unlock (persist offers into the next reroll), weapon recycle (`base_shop.gd:706-745`), combine parity (`:622-704`), ban (`:23,113-115`), level-up reroll (`:175-199`), crate take/recycle instead of auto-take (`upgrades_ui.gd:99-115`) + the new state fields 165..172 read by this obs |
| PolicyHarness | confirm obs 161 -> 198 and BT_ACTIONS 21 -> 31 (one masked discrete head) so the sim/bridge/policy sizes change together |
| LinuxGame / bridge | press the game's endless button on wave 19/20; drive the new actions through the game's own handlers (lock button, discard, ban, level-up reroll, crate take/discard); mirror obs 161..197 in `rl_obs.gd` |
| me (this audit) | DONE: obs 161..164 + 174..197, `BT_OBS_SIZE_PINNED`, `BT_ACTIONS` 31 + id table + `bt_action_mask` in the sim schema, mirror in `rl_obs.gd`, `state_contract.json` v4, tests; 165..173 stay reserved until AuditShop lands the state |

LANDED (status at the time of writing):
* sim schema `brotato_state.h`: `BT_OBS_SIZE` 161 -> 198 (composed from
  `BT_OBS_T_STATS 4` + `T_LOCKS 4` + `T_CRATE 4` + `T_LVLRR 1` + `T_ITEMS 24`),
  `BT_OBS_SIZE_PINNED 198` (a literal emitted by `gen_obs_tables.py` so the
  bridge can assert the width), `BT_ACTIONS` 21 -> 31 and the `BT_ACT_*` id
  table.
* sim writer `brotato_core.h`: slots 161..164 and 174..197 are written;
  165..173 are reserved (the vector is memset to 0 first, so they read 0.0);
  `bt_action_mask(s, mask)` publishes the per-phase mask; `brotato.h`
  `puf_reset`/`puf_step` fill `agents[0].action_mask`.
* bridge `rl_obs.gd`: `OBS_SIZE 198`, the same block (harvesting/luck from
  `Utils.get_stat`, endless from `RunData.get_endless_factor()`, counts from
  `RunData.get_nb_item(id.hash(), 0)`), drift guards against
  `BT_OBS_SIZE_PINNED` / `BT_OBS_PROJ_MAX_D2` / `BT_ACTIONS`, plus the
  non-finite enemy skip (LinuxGame's live-arena report).
* contract `state_contract.json` v4: 198 floats / 31 actions, new ranges,
  `obs_version = 4`, and the acceptance gate treats a 161-wide recording as a
  v3 trace to compare on the first 161 fields.
* tests: `tests/test_brotato_weapon_observe.cpp` 96 checks / 0 failures
  (width, block, mask), `tests/test_brotato_core.cpp` 252 / 0.

Bridge policy mask: rl_policy.gd reads the artifact's `action_mask` clause
(`legal_by_phase` + `phase_field_index` + `selectors`) and derives each step's
mask from obs[phase_field_index], with the selector replacing `2_level_up` by
`2_crate` while obs[169] (crate kind) is nonzero - never a hard-coded index. The
sim's bt_action_mask makes 29/30 the ONLY legal ids while a box is open, so the
crate clause must REPLACE 0..4, not add to them.

STILL OPEN (other owners): endless waves (EndlessMode), the 165..173 state and
the new action mechanics (AuditShop), the GPU action mask (CudaParity - pufferl
gives GPU envs no host-side mask output; the CPU path is wired), the two stale
bridge constants `rl_inject.gd:30` / `rl_trace.gd:22` (LinuxGame).
