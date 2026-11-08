/* brotato_weapon_inventory.h -- the six-slot player weapon loadout.
 *
 * OWNERSHIP / CONTRACT
 *   This module owns the player's weapon loadout: the per-slot identity
 *   arrays (which canonical weapon copy sits in which slot), per-copy curse,
 *   the source instance uid, the tracked-value counter, and every lifecycle
 *   operation on them -- add / remove / remove-all / combine / upgrade /
 *   buy-with-auto-combine / recycle-value / tier+curse rules / purchase
 *   eligibility / starting-weapon enqueue+apply. It does NOT own effective
 *   weapon stats (WeaponStats), attack scheduling or melee/ranged runtime state
 *   (WeaponMelee / WeaponRanged), projectile ownership (WeaponProjectiles),
 *   weapon effect events (WeaponEffects), shop offers/pricing orchestration
 *   (WeaponShop) or class profiles (WeaponCharacters).
 *
 *   The identity arrays live in BtState because CPU/CUDA/Metal all step the same
 *   POD; the exact fields this file reads and writes are:
 *
 *     #define BT_WEAPON_SLOTS 6            (brotato_weapon_types.h)
 *     #define BT_WEAPON_START_MAX 8
 *     #define BT_WEAPON_UID_NONE 0xFFFFFFFFu
 *     unsigned short wpn_id[BT_WEAPON_SLOTS];       dense 0..wpn_count-1, BT_WEAPON_NONE = empty
 *     unsigned char  wpn_tier[BT_WEAPON_SLOTS];     per-copy BT_WEAPONS[id].tier (kept as real per-instance state)
 *     unsigned char  wpn_cursed[BT_WEAPON_SLOTS];   per-copy WeaponData.is_cursed
 *     float          wpn_curse[BT_WEAPON_SLOTS];    per-copy AGGREGATE curse_factor (max over the
 *                                                   weapon-stats draw and every effect draw)
 *     float          wpn_wmod[BT_WEAPON_SLOTS];     per-copy weapon-stats curse modifier (draw 1)
 *     float          wpn_fxcurse[BT_WEAPON_SLOTS][BT_WEAPON_FX_CURSE_MAX];
 *                                                   per top-level effect curse modifier, effects order
 *     float          wpn_cd[BT_WEAPON_SLOTS];       remaining attack cooldown, TICKS (Melee/Ranged own it)
 *     int            wpn_tracked[BT_WEAPON_SLOTS];  WeaponData.tracked_value
 *     unsigned int   wpn_uid[BT_WEAPON_SLOTS];      stable per-instance uid (BT_WEAPON_UID_NONE = empty)
 *     unsigned int   wpn_uid_next;                  next uid to mint (monotonic from 0; reset only in bt_core_reset)
 *     unsigned char  wpn_set_tier[BT_NWEAPON_SETS]; applied class-set tier per set, 0xFF = none (WeaponStats owns it)
 *     unsigned char  wpn_count, wpn_overflow, wpn_dirty;
 *     unsigned short wpn_start_id[BT_WEAPON_START_MAX];
 *     unsigned char  wpn_start_cursed[BT_WEAPON_START_MAX];
 *     unsigned char  wpn_start_count, wpn_start_overflow;
 *
 *   Slots are DENSE and in source order (RunData.get_player_weapons); removal
 *   compacts the tail down, exactly like the source `weapons.remove(index)`.
 *   Because compaction renumbers, any module holding a per-slot runtime array
 *   must move it through the shared hook below, and any module holding a slot
 *   index across a mutation must re-resolve it (or use wpn_uid, which travels
 *   with the copy and is never reused within a run).
 *
 *   Required class-effect scalars are read from s->scalar_effect (the canonical
 *   Effect storage; bt_apply_item writes them, WeaponCharacters seeds the
 *   profile defaults): BT_ISTR_WEAPON_SLOT, BT_ISTR_MIN_WEAPON_TIER,
 *   BT_ISTR_MAX_WEAPON_TIER, BT_ISTR_NO_RANGED_WEAPONS,
 *   BT_ISTR_NO_DUPLICATE_WEAPONS, BT_ISTR_MAX_MELEE_WEAPONS,
 *   BT_ISTR_MAX_RANGED_WEAPONS, BT_ISTR_WEAPONS_PRICE, BT_ISTR_ITEMS_PRICE,
 *   BT_ISTR_RECYCLING_GAINS.
 *
 *   SET BONUSES: every loadout mutation re-derives the class-set bonuses by
 *   calling bt_weapon_set_refresh() (brotato_weapon_set_runtime.h, WeaponStats)
 *   exactly where the source calls update_sets() -- after each add (before the
 *   copy's runtime state exists, so the initial resolved cooldown already
 *   includes the new set tier) and after each remove. bt_weapon_reset clears
 *   wpn_set_tier[] to 0xFF without unapplying anything, so a reset never folds
 *   stale bonuses into the freshly zeroed stat layers.
 *
 *   INCLUDE ORDER: include this header from brotato_core.h AFTER BtState,
 *   BT_WEAPONS (brotato_weapons.h) and bt_curse_modifier() are defined -- e.g.
 *   next to the brotato_loot.h/progression include block.
 *
 * SOURCE
 *   singletons/run_data.gd        add_weapon / remove_weapon /
 *                                 remove_weapon_by_index / remove_all_weapons /
 *                                 get_free_weapon_slots / has_weapon_slot_available
 *                                 / can_combine / get_unique_weapon_ids /
 *                                 add_starting_items_and_weapons
 *   ui/menus/shop/base_shop.gd    buy_weapon (602-615), _combine_weapon (631-703),
 *                                 _on_item_discard_button_pressed (706-748)
 *   singletons/item_service.gd    get_value (701-719), get_recycling_value (732-742),
 *                                 get_upgrades / weapon tier clamp (309-311),
 *                                 no_ranged_weapons filter (341-354)
 *   singletons/player_run_data.gd init_stats defaults (weapon_slot 6, max_weapon_tier
 *                                 99, max_melee/ranged_weapons 999)
 *   dlcs/dlc_1/dlc_1_data.gd      curse_item / _get_cursed_item_effect_modifier
 *   items/characters/bull/bull_effect_5.tres   weapon_slot 0 (REPLACE) -> Bull stays weaponless
 *
 * NAMED DEVIATIONS (not silent fallbacks)
 *   * `specific_items_price` (a KEY_VALUE bucket keyed on item-name substrings,
 *     base_shop.gd:718-731 / item_service.gd:709-713): the ONLY two producers in
 *     the shipped data are character effects -- fisherman_effect_2.tres
 *     (key "item_bait", value -100) and dlc_1 diver_effect_1.tres
 *     (key "weapon_harpoon_gun", value -100) -- and neither Fisherman nor Diver
 *     is a selectable profile (Bull/Knight/Demon only), so no reachable producer
 *     exists for this build. The core additionally has no KEY_VALUE bucket for
 *     the key (bt_apply_item drops unmapped KEY_VALUE rows), so the factor is
 *     1.0, the same omission bt_shop_price already makes for items. The exact
 *     fold point is provided: bt_weapon_recycle_value_from(s, id, base) takes the
 *     already-multiplied base value (the source get_recycling_value(from_value)
 *     shape). The world-level `items_price` / `weapons_price` / `recycling_gains`
 *     scalars ARE applied, in source order.
 *   * Curse is stored per copy exactly as the source persists it: wpn_wmod is
 *     the weapon-stats modifier (dlc_1_data.gd:58-60), wpn_fxcurse[e] the
 *     modifier of top-level effect e (dlc_1_data.gd:62-67), and wpn_curse the
 *     aggregate curse_factor = max over all of them (dlc_1_data.gd:344-348),
 *     which the combine floor consumes. No shipped weapon template is cursed (0
 *     of 270 weapon resources carry is_cursed = true), so curse_item never takes
 *     its early-return branch.
 *   * Slot capacity is clamped to BT_WEAPON_SLOTS(6): characters whose
 *     weapon_slot exceeds 6 (e.g. Multitasker) are outside the shared 6-record
 *     observation contract.
 *   * Price/recycle: for waves <= BT_WAVES (20) the source's endless factor is 0,
 *     so the campaign/Abyss path never sees it; the term is implemented anyway
 *     for waves past 20.
 */
#pragma once

#include "brotato_rng.h"            /* BT_HD / BT_DEVICE / BT_CONST_PTR / BT_TABLE */
#include "brotato_weapon_types.h"   /* BtWeaponDef, BT_WEAPON_NONE/SLOTS/KIND_* */
#include "brotato_weapons.h"        /* BT_WEAPONS, BT_NWEAPONS */
#include "brotato_item_effects.h"   /* BT_ISTR_* interned scalars */

/* Starting-weapon queue bound. The source adds starting weapons from the
 * character's `starting_weapon` KEY_VALUE effects plus one chosen from the
 * character's pool, each exactly once per shipped profile (7 such effect rows
 * exist, all value 1), and a usable loadout is at most `weapon_slot` (<= 6). 8
 * is a genuine bound for the shipped data; exceeding it sets wpn_start_overflow
 * (explicit, never a silently lost equip). */
#ifndef BT_WEAPON_START_MAX
#define BT_WEAPON_START_MAX 8
#endif

/* Per-copy storage for the curse modifier of each top-level weapon effect
 * (dlc_1_data.gd:62-67 draws one per effect, in effects order). Measured over
 * every shipped weapon resource: 99 have 0 top-level effects, 155 have 1, 16
 * have 2, none has more (270 resources across campaign + DLC). A weapon that
 * exceeded this would still get its draws (RNG parity) and would set
 * wpn_overflow instead of silently dropping a modifier. */
#ifndef BT_WEAPON_FX_CURSE_MAX
#define BT_WEAPON_FX_CURSE_MAX 2
#endif

/* BtState declares wpn_fxcurse[BT_WEAPON_SLOTS][<dim>]; this module's
 * BT_WEAPON_FX_CURSE_MAX must be that same dimension. A mismatch is a compile
 * error here rather than a silent truncation of a copy's effect modifiers. */
typedef char bt_weapon_fxcurse_dim_check
    [(BT_WEAPON_FX_CURSE_MAX ==
      (int)(sizeof(((BT_DEVICE BtState*)0)->wpn_fxcurse[0]) / (int)sizeof(float))) ? 1 : -1];

/* Class-set bookkeeping sentinel (brotato_weapon_set_runtime.h uses 0xFF for
 * "no tier applied"). */
#ifndef BT_WEAPON_SET_TIER_NONE
#define BT_WEAPON_SET_TIER_NONE 0xFF
#endif


/* Slot identity: 0xFFFFFFFF is the "none" sentinel shared with
 * WeaponProjectiles' owner_uid; uids are minted monotonically from 0, are never
 * reused within an episode, and reset to 0 only when bt_core_reset clears the
 * projectile/effect queues that may still hold them. */
#ifndef BT_WEAPON_UID_NONE
#define BT_WEAPON_UID_NONE 0xFFFFFFFFu
#endif

/* ---------------------------------------------------------------------------
 * Per-slot runtime hooks, owned by WeaponMelee (BT_WEAPON_MELEE_HOOKS),
 * WeaponRanged (BT_WEAPON_RANGED_HOOKS) and WeaponEffects (BT_WEAPON_FX_HOOKS).
 * Each module keeps its own per-slot runtime arrays (melee swing/animation/hit
 * masks, ranged burst/cooldown state, weapon-effect per-wave kill counters)
 * indexed by the SAME dense slot index; Melee and Ranged also own the shared
 * wpn_cd[BT_WEAPON_SLOTS] array (float TICKS) for their own kind, so a hook must
 * no-op when `bt_weapon_kind(s->wpn_id[slot])` is not its kind.
 *
 *   slot_reset(s, slot)  the copy that just landed in `slot` gets its runtime
 *                        state created -- source Weapon._ready ->
 *                        init_stats(true) -> get_next_cooldown(true), which
 *                        consumes exactly ONE rand_range draw. This module calls
 *                        it once per insert, in source add order, and writes
 *                        wpn_cd[slot] = 0 first (weapon.gd `_current_cooldown = 0`
 *                        before `_ready`); the hook overwrites it with the drawn
 *                        cooldown. NEVER called from bt_weapon_reset.
 *                        (Melee/Ranged only: WeaponEffects has no creation step.)
 *                        The draw's weapon_count bound is the copy's SLOT INDEX:
 *                        _ready runs inside add_child, before player.gd:387-389
 *                        pushes the node onto current_weapons, so
 *                        player.get_nb_weapons() excludes the copy being created.
 *   slot_move(s, from, to)  dense compaction copied slot `from` onto slot `to`
 *                        (to < from) -- move the runtime arrays, no draws.
 *   slot_erase(s, slot)  slot `slot` was vacated -- zero the runtime arrays
 *                        (wpn_cd included), no draws.
 *   forget_uid(s, uid)  the copy carrying `uid` CEASED TO EXIST (remove,
 *                        remove_all, combine, upgrade). Called only for the
 *                        removed copy's uid, never for a copy that merely moved
 *                        down the tail, so a relocated copy's history is never
 *                        destroyed. WeaponEffects only (its attack-history
 *                        block); no draw.
 *
 * This module shifts its own identity and curse arrays (wpn_id/_tier/_cursed/
 * _curse/_wmod/_fxcurse/_tracked/_uid) and wpn_cd before/independently of the
 * hooks; a hook must move ONLY its own arrays so nothing is shifted twice.
 * ------------------------------------------------------------------------- */
#if defined(BT_WEAPON_MELEE_HOOKS)
BT_HD static void bt_weapon_melee_slot_reset(BT_DEVICE BtState* s, int slot);
BT_HD static void bt_weapon_melee_slot_move(BT_DEVICE BtState* s, int from, int to);
BT_HD static void bt_weapon_melee_slot_erase(BT_DEVICE BtState* s, int slot);
#endif
/* The wave-begin node re-creation for one melee copy, defined later in the TU
 * (brotato_weapon_melee.h).  Same body as the copy's slot_reset, so it is
 * emitted outside the BT_WEAPON_MELEE_HOOKS guard like its ranged counterpart. */
BT_HD static void bt_weapon_melee_wave_begin_slot(BT_DEVICE BtState* s, int slot);
#if defined(BT_WEAPON_RANGED_HOOKS)
BT_HD static void bt_weapon_ranged_slot_reset(BT_DEVICE BtState* s, int slot);
BT_HD static void bt_weapon_ranged_slot_move(BT_DEVICE BtState* s, int from, int to);
BT_HD static void bt_weapon_ranged_slot_erase(BT_DEVICE BtState* s, int slot);
#endif
/* WeaponEffects owns BtWeaponFxState (weapon.gd `_enemies_killed_this_wave_count`,
 * per slot) and has no per-copy creation step, so it contributes move/erase only.
 * Defined under BT_WEAPON_FX_HOOKS; the natural signatures take BtWeaponFxState*,
 * so this module calls the BtState-based wrappers below. */
#if defined(BT_WEAPON_FX_HOOKS)
BT_HD static void bt_weapon_fx_slot_move(BT_DEVICE BtState* s, int from, int to);
BT_HD static void bt_weapon_fx_slot_erase(BT_DEVICE BtState* s, int slot);
BT_HD static void bt_weapon_fx_forget_uid(BT_DEVICE BtState* s, unsigned int uid);
#endif

/* Dispatch to every enabled runtime owner. Without the macros there are no
 * extra per-slot arrays, so nothing can be left stale. */
BT_HD static void bt_weapon_rt_reset(BT_DEVICE BtState* s, int slot) {
#if defined(BT_WEAPON_MELEE_HOOKS)
    bt_weapon_melee_slot_reset(s, slot);
#endif
#if defined(BT_WEAPON_RANGED_HOOKS)
    bt_weapon_ranged_slot_reset(s, slot);
#endif
}
BT_HD static void bt_weapon_rt_move(BT_DEVICE BtState* s, int from, int to) {
#if defined(BT_WEAPON_MELEE_HOOKS)
    bt_weapon_melee_slot_move(s, from, to);
#endif
#if defined(BT_WEAPON_RANGED_HOOKS)
    bt_weapon_ranged_slot_move(s, from, to);
#endif
#if defined(BT_WEAPON_FX_HOOKS)
    bt_weapon_fx_slot_move(s, from, to);
#endif
}
/* A copy ceased to exist: its owner drops the per-uid history block. Called
 * exactly once per disappeared copy, with the uid it had while alive. */
BT_HD static void bt_weapon_rt_forget(BT_DEVICE BtState* s, unsigned int uid) {
#if defined(BT_WEAPON_FX_HOOKS)
    if (uid != BT_WEAPON_UID_NONE) bt_weapon_fx_forget_uid(s, uid);
#else
    (void)s; (void)uid;
#endif
}
BT_HD static void bt_weapon_rt_erase(BT_DEVICE BtState* s, int slot) {
#if defined(BT_WEAPON_MELEE_HOOKS)
    bt_weapon_melee_slot_erase(s, slot);
#endif
#if defined(BT_WEAPON_RANGED_HOOKS)
    bt_weapon_ranged_slot_erase(s, slot);
#endif
#if defined(BT_WEAPON_FX_HOOKS)
    bt_weapon_fx_slot_erase(s, slot);
#endif
}

/* --- forward declarations of core helpers this module calls ---------------- */
/* bt_curse_modifier() (brotato_core.h, dlc_1_data.gd:353-357) is the single
 * cursed-item modifier draw; it is defined before this header is included. */
BT_HD static float bt_curse_modifier(BT_DEVICE BtState* s);
/* bt_weapon_set_refresh() (brotato_weapon_set_runtime.h, WeaponStats; included by
 * brotato_core.h after the stat helpers) re-derives the class-set bonuses from
 * wpn_id[]. Forward declared here, defined later in the same TU -- the same
 * cross-module pattern bt_curse_modifier uses. */
BT_HD static void bt_weapon_set_refresh(BT_DEVICE BtState* s);
/* Item-related weapon bonuses (brotato_item_weapon_bonus.h, WeaponStats;
 * included by brotato_core.h right after brotato_weapon_set_runtime.h).
 * bt_weapon_insert registers the new copy's bucket effects and re-derives the
 * bonuses (run_data.gd:914 add_weapon -> apply_item_effects ->
 * update_item_related_effects); bt_weapon_erase unregisters the removed copy's
 * and re-derives (:1000-1009 after_weapon_removed).  Forward declared so the
 * inventory mutations below compile before the definition, exactly like
 * bt_weapon_set_refresh.  `fxcurse` is the slot's per-top-level-effect curse
 * modifier array, or NULL when the copy is not cursed. */
BT_HD static void bt_iwb_register_weapon(BT_DEVICE BtState* s, unsigned short id,
                                         const float* fxcurse);
BT_HD static void bt_iwb_unregister_weapon(BT_DEVICE BtState* s, unsigned short id,
                                           const float* fxcurse);
BT_HD static void bt_item_wbonus_refresh(BT_DEVICE BtState* s);
/* The stat-layer half of the same apply_item_effects / unapply_item_effects pair
 * (brotato_item_weapon_bonus.h, defined later in the TU): every weapon's own
 * SUM-storage `stat_*` effect rows, applied at bt_weapon_insert and unapplied at
 * bt_weapon_erase.  `dir` = +1 / -1. */
BT_HD static void bt_weapon_statfx_apply(BT_DEVICE BtState* s, unsigned short id,
                                         const float* fxcurse, int dir);
/* Wave-begin node re-creation for one copy, defined later in the TU
 * (brotato_weapon_ranged.h / brotato_weapon_melee.h).  Both halves are
 * per-slot and slot-ordered: the source create loop is global, so a mixed
 * loadout draws in slot order, one get_next_cooldown(true) draw per weapon. */
#if defined(BT_WEAPON_RANGED_HOOKS)
BT_HD static void bt_ranged_wave_begin_slot(BT_DEVICE BtState* s, int slot);
#endif

/* A wave boundary loads a FRESH game scene (`base_shop.gd:345-346`
 * `RunData.current_wave += 1` then `get_tree().change_scene(MenuData.game_scene)`;
 * main.gd:1134), so the player node and every Weapon node are re-created from the
 * persistent RunData loadout (player.gd:315-320 apply_items_effects ->
 * player.gd:376-390 add_weapon -> weapon.gd:69-88 _ready ->
 * weapon.gd:137-142 init_stats(at_wave_begin = true) -> get_next_cooldown(true),
 * ONE rand_range draw per weapon in slot order, then reset_cooldown()).
 *
 * Model: reset each copy's runtime state and draw its fresh wave-begin cooldown,
 * slot by slot, so the per-weapon draw order and the weapon_count bound
 * (`player.get_nb_weapons()` == the number of nodes already created == slot) are
 * both the source's.  `player.gd:269 reset_weapons_cd` is NOT this: it has no
 * caller in the recovered sources.
 *
 * Both kinds are per-slot now: a mixed loadout draws melee and ranged copies in
 * the same slot order the node creation loop has. */
BT_HD static void bt_weapon_wave_begin(BT_DEVICE BtState* s) {
    for (int slot = 0; slot < (int)s->wpn_count && slot < BT_SLOTS; slot++) {
        unsigned short wid = s->wpn_id[slot];
        if (wid == BT_WEAPON_NONE || wid >= BT_NWEAPONS) continue;
        if (BT_WEAPONS[wid].kind == BT_WEAPON_KIND_MELEE) {
            bt_weapon_melee_wave_begin_slot(s, slot);
            continue;
        }
#if defined(BT_WEAPON_RANGED_HOOKS)
        if (BT_WEAPONS[wid].kind == BT_WEAPON_KIND_RANGED) {
            bt_ranged_wave_begin_slot(s, slot);
            continue;
        }
#endif
    }
}

/* ---------------------------------------------------------------------------
 * Generated-table adapters (brotato_weapons.h). Index-based so they stay
 * device-portable; every invalid id returns the neutral value, never reads row 0.
 * ------------------------------------------------------------------------- */
BT_HD static int bt_weapon_valid(unsigned short id) { return id < (unsigned short)BT_NWEAPONS; }
BT_HD static int bt_weapon_tier(unsigned short id) { return bt_weapon_valid(id) ? (int)BT_WEAPONS[id].tier : -1; }
BT_HD static int bt_weapon_kind(unsigned short id) { return bt_weapon_valid(id) ? (int)BT_WEAPONS[id].kind : -1; }
BT_HD static unsigned short bt_weapon_upgrades(unsigned short id) {
    return bt_weapon_valid(id) ? BT_WEAPONS[id].upgrades_into : BT_WEAPON_NONE;
}
/* Canonical family row (run_data.gd:1075-1082 unique_weapon_ids is keyed by
 * WeaponData.weapon_id; BtWeaponDef.family is the lowest-tier row of that id). */
BT_HD static unsigned short bt_weapon_family(unsigned short id) {
    return bt_weapon_valid(id) ? BT_WEAPONS[id].family : BT_WEAPON_NONE;
}

/* ---------------------------------------------------------------------------
 * Class / profile restrictions (source Effect scalars).
 *
 * Source defaults (PlayerRunData.init_stats / init_effects): weapon_slot 6
 * (Bull replaces it with 0 via bull_effect_5), max_weapon_tier 99,
 * max_melee_weapons / max_ranged_weapons 999, min_weapon_tier 0,
 * no_ranged_weapons / no_duplicate_weapons 0, weapons_price 0.
 * Character initialization seeds max_weapon_tier explicitly; zero is a valid
 * upper tier, not an absent-value sentinel. The kind-count caps retain their
 * source defaults where no selected profile overrides them.
 * ------------------------------------------------------------------------- */
BT_HD static int bt_weapon_slot_cap(BT_DEVICE BtState* s) {
    int v = (int)s->scalar_effect[BT_ISTR_WEAPON_SLOT];
    if (v < 0) v = 0;
    if (v > BT_WEAPON_SLOTS) v = BT_WEAPON_SLOTS;   /* 6-record observation contract */
    return v;
}
BT_HD static int bt_weapon_min_tier(BT_DEVICE BtState* s) {
    return (int)s->scalar_effect[BT_ISTR_MIN_WEAPON_TIER];
}
BT_HD static int bt_weapon_max_tier(BT_DEVICE BtState* s) {
    return (int)s->scalar_effect[BT_ISTR_MAX_WEAPON_TIER];
}
BT_HD static int bt_weapon_kind_cap(BT_DEVICE BtState* s, int kind) {
    float f = (kind == BT_WEAPON_KIND_RANGED) ? s->scalar_effect[BT_ISTR_MAX_RANGED_WEAPONS]
                                              : s->scalar_effect[BT_ISTR_MAX_MELEE_WEAPONS];
    int v = (int)f;
    return v == 0 ? 999 : v;                         /* source default */
}
BT_HD static int bt_weapon_no_ranged(BT_DEVICE BtState* s) { return s->scalar_effect[BT_ISTR_NO_RANGED_WEAPONS] != 0.0f; }
BT_HD static int bt_weapon_no_duplicate(BT_DEVICE BtState* s) { return s->scalar_effect[BT_ISTR_NO_DUPLICATE_WEAPONS] != 0.0f; }
/* item_service.gd:309-311 generator clamp. */
BT_HD static int bt_weapon_tier_clamp(BT_DEVICE BtState* s, int tier) {
    int lo = bt_weapon_min_tier(s), hi = bt_weapon_max_tier(s);
    if (tier < lo) tier = lo;
    if (tier > hi) tier = hi;
    return tier;
}
/* item_service.gd:341-354 + shop_items_container.gd:96-139: no_ranged_weapons
 * (Knight knight_effect_3) and no_melee_weapons remove that kind from the
 * generated pool / buy gate. `no_melee_weapons` is a real source key (Ranger
 * ranger_effect_5, Renegade renegade_effect_2) but no Bull/Knight/Demon profile
 * carries it, so that key is optional: when the interner has not emitted it, no
 * effect can express it and the gate is correctly absent rather than silently
 * wrong. */
BT_HD static int bt_weapon_shop_kind_ok(BT_DEVICE BtState* s, unsigned short id) {
    int kind = bt_weapon_kind(id);
    if (kind == BT_WEAPON_KIND_RANGED && bt_weapon_no_ranged(s)) return 0;
#ifdef BT_ISTR_NO_MELEE_WEAPONS
    if (kind == BT_WEAPON_KIND_MELEE && s->scalar_effect[BT_ISTR_NO_MELEE_WEAPONS] != 0.0f) return 0;
#endif
    return 1;
}

/* ---------------------------------------------------------------------------
 * Loadout reads.
 * ------------------------------------------------------------------------- */
BT_HD static int bt_weapon_count(BT_DEVICE BtState* s) { return (int)s->wpn_count; }

BT_HD static unsigned short bt_weapon_id(BT_DEVICE BtState* s, int slot) {
    if (slot < 0 || slot >= (int)s->wpn_count) return BT_WEAPON_NONE;
    return s->wpn_id[slot];
}
BT_HD static int bt_weapon_slot_cursed(BT_DEVICE BtState* s, int slot) {
    if (slot < 0 || slot >= (int)s->wpn_count) return 0;
    return s->wpn_cursed[slot] ? 1 : 0;
}
BT_HD static unsigned int bt_weapon_uid(BT_DEVICE BtState* s, int slot) {
    if (slot < 0 || slot >= (int)s->wpn_count) return BT_WEAPON_UID_NONE;
    return s->wpn_uid[slot];
}
/* -1 when no live copy carries `uid` (e.g. the copy was recycled/combined, or a
 * projectile still points at a uid from before the last reset). */
BT_HD static int bt_weapon_slot_from_uid(BT_DEVICE BtState* s, unsigned int uid) {
    if (uid == BT_WEAPON_UID_NONE) return -1;
    for (int i = 0; i < (int)s->wpn_count; i++) if (s->wpn_uid[i] == uid) return i;
    return -1;
}
/* First owned slot whose canonical variant id is `id`, or -1. */
BT_HD static int bt_weapon_slot_of(BT_DEVICE BtState* s, unsigned short id) {
    for (int i = 0; i < (int)s->wpn_count; i++) if (s->wpn_id[i] == id) return i;
    return -1;
}
/* Copies of one canonical variant (can_combine counts the weapon itself too). */
BT_HD static int bt_weapon_copies_of(BT_DEVICE BtState* s, unsigned short id) {
    int n = 0;
    for (int i = 0; i < (int)s->wpn_count; i++) if (s->wpn_id[i] == id) n++;
    return n;
}
/* Copies of one family (run_data.gd get_unique_weapon_ids keys on weapon_id). */
BT_HD static int bt_weapon_family_copies(BT_DEVICE BtState* s, unsigned short family) {
    int n = 0;
    for (int i = 0; i < (int)s->wpn_count; i++) if (bt_weapon_family(s->wpn_id[i]) == family) n++;
    return n;
}
/* Distinct families owned (get_unique_weapon_ids().size()). */
BT_HD static int bt_weapon_unique_families(BT_DEVICE BtState* s) {
    int n = 0;
    for (int i = 0; i < (int)s->wpn_count; i++) {
        unsigned short f = bt_weapon_family(s->wpn_id[i]);
        int seen = 0;
        for (int j = 0; j < i; j++) if (bt_weapon_family(s->wpn_id[j]) == f) { seen = 1; break; }
        if (!seen) n++;
    }
    return n;
}
/* Copies of one kind (0 melee / 1 ranged). */
BT_HD static int bt_weapon_kind_copies(BT_DEVICE BtState* s, int kind) {
    int n = 0;
    for (int i = 0; i < (int)s->wpn_count; i++) if (bt_weapon_kind(s->wpn_id[i]) == kind) n++;
    return n;
}
/* ---------------------------------------------------------------------------
 * Per-copy curse (dlc_1_data.gd:48-357 curse_item).
 *
 * curse_item(item, player, turn_randomization_off, min_modifier) draws, in this
 * exact order, ONE modifier for the weapon's own stats (line 58) and then one
 * for EVERY top-level effect (line 63), each
 *   max(min_modifier, (base + inc*min(20, wave-1) + randi_range(-30,30)) / 100)
 * and stores the copy's curse_factor as the MAXIMUM over all of them (line 348).
 * Every draw goes through Utils.randi_range == s->utils_rng, so the sequence
 * never touches the node rng stream Weapon._ready's get_next_cooldown uses.
 *
 * This module owns that sequence because it owns copy creation:
 *   wpn_wmod[slot]        the weapon-stats modifier (draw 1) -- what WeaponStats
 *                         applies to the UNCURSED base weapon stats;
 *   wpn_fxcurse[slot][e]  effect e's modifier, in effects order, for the effect
 *                         owner (WeaponEffects/WeaponStats);
 *   wpn_curse[slot]       the aggregate curse_factor (max over every draw) --
 *                         what this module's combine floor consumes.
 * turn_randomization_off pins random_modifier AND wave_basis to 0, so
 * min(20, 0-1) == -1 and every modifier is the constant
 * (base + inc*(-1))/100 = 0.38; that path draws nothing at all.
 * ------------------------------------------------------------------------- */
#define BT_WEAPON_CURSE_DET_MOD \
    ((BT_DLC1_CURSE_CURSED_ITEM_BASE_PERCENT_MODIFIER - \
      BT_DLC1_CURSE_CURSED_ITEM_PERCENT_MODIFIER_INCREASE_EACH_WAVE) / 100.0f)

/* One curse_item draw with the source's min_modifier floor. bt_curse_modifier()
 * performs the randi_range(-30,30) draw on utils_rng and floors p at 0; the
 * source's max() against a non-negative min_modifier yields the same value. */
BT_HD static float bt_weapon_curse_draw(BT_DEVICE BtState* s, float floor_mod) {
    float m = bt_curse_modifier(s);
    return m > floor_mod ? m : floor_mod;
}

/* One copy's curse state, produced BEFORE the copy is inserted so
 * bt_weapon_insert can write identity + curse, re-derive the class sets and only
 * then create the runtime state, in source order (curse_item -> add_weapon ->
 * Weapon._ready). */
typedef struct { int cursed; float wmod, agg; float fx[BT_WEAPON_FX_CURSE_MAX]; } BtWeaponCurse;

BT_HD static void bt_weapon_curse_clear(BT_THREAD BtWeaponCurse* c) {
    c->cursed = 0;
    c->wmod = 0.0f;
    c->agg = 0.0f;
    for (int k = 0; k < BT_WEAPON_FX_CURSE_MAX; k++) c->fx[k] = 0.0f;
}

/* Roll a copy's curse exactly as curse_item does. `randomized` = 0 is the
 * turn_randomization_off path (cursed shop offers, cursed starting weapons): no
 * draws, and every modifier is `fixed` when the caller already holds the baked
 * offer value, else BT_WEAPON_CURSE_DET_MOD. The draws happen here, in source
 * order, before the copy exists. */
BT_HD static void bt_weapon_curse_roll(BT_DEVICE BtState* s, unsigned short id, int randomized,
                                       float fixed, float floor_mod,
                                       BT_THREAD BtWeaponCurse* out) {
    bt_weapon_curse_clear(out);
    if (!bt_weapon_valid(id)) return;
    float flat = fixed > 0.0f ? fixed : BT_WEAPON_CURSE_DET_MOD;
    float wmod = randomized ? bt_weapon_curse_draw(s, floor_mod) : flat;
    float agg = wmod;
    int fx = (int)BT_WEAPONS[id].fx_count;
    int storable = fx < BT_WEAPON_FX_CURSE_MAX ? fx : BT_WEAPON_FX_CURSE_MAX;
    if (fx > storable) s->wpn_overflow = 1;   /* draws kept for RNG parity, storage overflow is explicit */
    for (int e = 0; e < fx; e++) {
        float em = randomized ? bt_weapon_curse_draw(s, floor_mod) : flat;
        if (e < storable) out->fx[e] = em;
        if (em > agg) agg = em;
    }
    out->cursed = 1;
    out->wmod = wmod;
    out->agg = agg;
}

/* The copy's AGGREGATE curse_factor (dlc_1_data.gd:348) -- the combine floor and
 * the value the observation/UI reports. 0 for a non-cursed copy. */
BT_HD static float bt_weapon_curse_modifier(BT_DEVICE BtState* s, int slot) {
    if (slot < 0 || slot >= (int)s->wpn_count) return 0.0f;
    return s->wpn_cursed[slot] ? s->wpn_curse[slot] : 0.0f;
}
/* The weapon-stats modifier (dlc_1_data.gd:58-60): what WeaponStats feeds to
 * _boost_weapon_stats_damage. Never a fresh draw. */
BT_HD static float bt_weapon_stats_curse_modifier(BT_DEVICE BtState* s, int slot) {
    if (slot < 0 || slot >= (int)s->wpn_count) return 0.0f;
    return s->wpn_cursed[slot] ? s->wpn_wmod[slot] : 0.0f;
}
/* Top-level effect `index`'s curse modifier for this copy (effects order). */
BT_HD static float bt_weapon_fx_curse(BT_DEVICE BtState* s, int slot, int index) {
    if (slot < 0 || slot >= (int)s->wpn_count) return 0.0f;
    if (index < 0 || index >= BT_WEAPON_FX_CURSE_MAX) return 0.0f;
    if (!s->wpn_cursed[slot]) return 0.0f;
    return s->wpn_fxcurse[slot][index];
}

/* ---------------------------------------------------------------------------
 * Lifecycle mutations.
 *
 * Insert/erase are the only places the identity arrays move, and each calls the
 * runtime hooks exactly once per moved / vacated / created slot.
 * ------------------------------------------------------------------------- */
/* Insert a copy at `at` (append when at < 0 or at >= count), writing its
 * identity and the curse state rolled by bt_weapon_curse_roll (`c`, may be NULL
 * for an uncursed copy) -- the only place a copy is created, so the only writer
 * of wpn_id/_tier/_cursed/_curse/_wmod/_fxcurse/_tracked/_uid. Returns the slot,
 * or -1 when the id is invalid or the hard BT_WEAPON_SLOTS bound is hit (sets
 * wpn_overflow). Order: identity -> curse -> class sets -> runtime state.
 * wpn_cd starts at 0: weapon.gd `_current_cooldown = 0` at creation; the
 * wave-begin get_next_cooldown() belongs to Melee/Ranged. */
BT_HD static int bt_weapon_insert(BT_DEVICE BtState* s, int at, unsigned short id, int tracked,
                                  BT_THREAD const BtWeaponCurse* c) {
    if (!bt_weapon_valid(id)) { s->wpn_overflow = 1; return -1; }
    if ((int)s->wpn_count >= BT_WEAPON_SLOTS) { s->wpn_overflow = 1; return -1; }
    /* Uid exhaustion is an explicit error, never a wrap onto the sentinel or an
     * ABA reuse: 0xFFFFFFFF-1 acquisitions are needed to reach it. */
    if (s->wpn_uid_next == BT_WEAPON_UID_NONE) { s->wpn_overflow = 1; return -1; }
    int n = (int)s->wpn_count;
    if (at < 0 || at > n) at = n;
    for (int i = n; i > at; i--) {
        s->wpn_id[i] = s->wpn_id[i - 1];
        s->wpn_tier[i] = s->wpn_tier[i - 1];
        s->wpn_cursed[i] = s->wpn_cursed[i - 1];
        s->wpn_curse[i] = s->wpn_curse[i - 1];
        s->wpn_wmod[i] = s->wpn_wmod[i - 1];
        for (int c = 0; c < BT_WEAPON_FX_CURSE_MAX; c++)
            s->wpn_fxcurse[i][c] = s->wpn_fxcurse[i - 1][c];
        s->wpn_cd[i] = s->wpn_cd[i - 1];
        s->wpn_tracked[i] = s->wpn_tracked[i - 1];
        s->wpn_uid[i] = s->wpn_uid[i - 1];
        bt_weapon_rt_move(s, i - 1, i);   /* owned runtime arrays follow the copy */
    }
    /* Every acquisition mints a fresh uid, monotonically from 0, never reused
     * within an episode (2^32-2 acquisitions; exhaustion was rejected above). */
    unsigned int uid = s->wpn_uid_next;
    s->wpn_uid_next = s->wpn_uid_next + 1u;
    int cursed = (c && c->cursed) ? 1 : 0;
    s->wpn_id[at] = id;
    s->wpn_tier[at] = (unsigned char)bt_weapon_tier(id);
    s->wpn_cursed[at] = (unsigned char)cursed;
    s->wpn_curse[at] = cursed ? c->agg : 0.0f;
    s->wpn_wmod[at] = cursed ? c->wmod : 0.0f;
    for (int k = 0; k < BT_WEAPON_FX_CURSE_MAX; k++) s->wpn_fxcurse[at][k] = cursed ? c->fx[k] : 0.0f;
    s->wpn_cd[at] = 0.0f;
    s->wpn_tracked[at] = tracked;
    s->wpn_uid[at] = uid;
    s->wpn_count = (unsigned char)(n + 1);
    s->wpn_dirty = 1;   /* WeaponStats re-resolves its cached per-slot stats */
    /* add_weapon call order (run_data.gd:905-915): apply_item_effects (the
     * copy's stat-layer rows AND its item-related weapon-bonus bucket effects)
     * -> update_sets -> update_item_related_effects, all BEFORE Weapon._ready's
     * get_next_cooldown draw, so the new copy's contribution is already folded
     * into the statistics that draw resolves against. */
    bt_weapon_statfx_apply(s, id, cursed ? s->wpn_fxcurse[at] : 0, 1);
    bt_iwb_register_weapon(s, id, cursed ? s->wpn_fxcurse[at] : 0);
    /* A weapon's own effect rows join the player's effect aggregation exactly
     * like an item's (run_data.gd add_weapon -> apply_item_effects ->
     * update_item_related_effects), which is how the Pruner's garden effect
     * becomes an owned structure the StructureTimer spawns. */
    bt_weapon_register_summons(s, id, cursed ? s->wpn_wmod[at] : 0.0f);
    /* update_sets() BEFORE the copy's runtime state exists, so Weapon._ready's
     * get_next_cooldown(true) resolves against the statistics the new set tier
     * just changed (run_data.gd:905-915 add_weapon call order). */
    bt_weapon_set_refresh(s);
    bt_item_wbonus_refresh(s);
    /* The copy now exists at `at`; clear any stale runtime state left in that
     * slot (a middle insert can land on a shifted slot), then let the
     * kind-owning module create its runtime state (weapon.gd _ready ->
     * get_next_cooldown(true): ONE rand draw per added weapon, in source add
     * order). */
    bt_weapon_rt_erase(s, at);
    /* The kind-owning module creates the copy's RUNTIME state (Weapon._ready ->
     * init_stats(at_wave_begin = true) -> get_next_cooldown(true): ONE rand draw
     * per added weapon, in source add order) ONLY while the ARENA scene is live
     * (phase 0).  A copy bought in a shop or taken from a crate is added to the
     * persistent RunData loadout by its handler (`base_shop.gd` buy -> 
     * `RunData.add_weapon`, run_data.gd:905-915): the shop scene has no Player
     * node, so no Weapon node exists and `_ready` (the cooldown draw) happens at
     * the NEXT wave's scene build instead -- `player.gd:315-320
     * apply_items_effects -> add_weapon`, which this core runs as
     * bt_weapon_wave_begin/bt_character_starting_apply.  Drawing here charged
     * the shop its 3 words early: measured on v5_demon_s1, the game's tick 306
     * (a successful weapon buy) consumed 0 global words while this core consumed
     * 3 (one rand_range), shifting every later spawn position by that draw. */
    if (s->phase == 0) bt_weapon_rt_reset(s, at);
    return at;
}

/* Append a copy (source `weapons.push_back(new_weapon)`, run_data.gd:905-915).
 * `curse_mod` is the weapon-stats modifier to use when `cursed`; pass 0.0f for
 * the source's turn_randomization_off determinism (BT_WEAPON_CURSE_DET_MOD =
 * 0.38, applied to the weapon and to every effect) or the shop offer's baked
 * value. It is ignored when cursed == 0. */
BT_HD static int bt_weapon_add(BT_DEVICE BtState* s, unsigned short id, int cursed, float curse_mod) {
    BtWeaponCurse c;
    if (cursed) bt_weapon_curse_roll(s, id, 0, curse_mod, 0.0f, &c);
    else bt_weapon_curse_clear(&c);
    return bt_weapon_insert(s, -1, id, 0, &c);
}

/* Erase and compact the tail down (Array.remove, run_data.gd:981-1020).
 * Returns the removed canonical id, or BT_WEAPON_NONE when `slot` is out of
 * range. Callers that need the copy's tracked_value must read
 * wpn_tracked[slot] BEFORE calling (indices shift). */
BT_HD static int bt_weapon_erase(BT_DEVICE BtState* s, int slot) {
    if (slot < 0 || slot >= (int)s->wpn_count) return BT_WEAPON_NONE;
    unsigned short gone = s->wpn_id[slot];
    unsigned int gone_uid = s->wpn_uid[slot];   /* the only copy that ceases to exist here */
    /* Read the removed copy's curse state BEFORE the compaction overwrites the
     * slot: after_weapon_removed -> unapply_item_effects must drop the exact
     * item-related weapon-bonus pairs the copy registered at add_weapon. */
    int gone_cursed = (int)s->wpn_cursed[slot];
    float gone_fx[BT_WEAPON_FX_CURSE_MAX];
    for (int c = 0; c < BT_WEAPON_FX_CURSE_MAX; c++) gone_fx[c] = s->wpn_fxcurse[slot][c];
    int n = (int)s->wpn_count;
    for (int i = slot; i + 1 < n; i++) {
        s->wpn_id[i] = s->wpn_id[i + 1];
        s->wpn_tier[i] = s->wpn_tier[i + 1];
        s->wpn_cursed[i] = s->wpn_cursed[i + 1];
        s->wpn_curse[i] = s->wpn_curse[i + 1];
        s->wpn_wmod[i] = s->wpn_wmod[i + 1];
        for (int c = 0; c < BT_WEAPON_FX_CURSE_MAX; c++)
            s->wpn_fxcurse[i][c] = s->wpn_fxcurse[i + 1][c];
        s->wpn_cd[i] = s->wpn_cd[i + 1];
        s->wpn_tracked[i] = s->wpn_tracked[i + 1];
        s->wpn_uid[i] = s->wpn_uid[i + 1];
        bt_weapon_rt_move(s, i + 1, i);   /* owned runtime arrays follow the copy */
    }
    s->wpn_count = (unsigned char)(n - 1);
    s->wpn_id[n - 1] = BT_WEAPON_NONE;
    s->wpn_tier[n - 1] = 0;
    s->wpn_cursed[n - 1] = 0;
    s->wpn_curse[n - 1] = 0.0f;
    s->wpn_wmod[n - 1] = 0.0f;
    for (int c = 0; c < BT_WEAPON_FX_CURSE_MAX; c++) s->wpn_fxcurse[n - 1][c] = 0.0f;
    s->wpn_cd[n - 1] = 0.0f;
    s->wpn_tracked[n - 1] = 0;
    s->wpn_uid[n - 1] = BT_WEAPON_UID_NONE;
    bt_weapon_rt_erase(s, n - 1);         /* vacated slot: zero the runtime arrays */
    /* The removed copy is the ONLY uid that disappears: the copies that smeared
     * down the tail keep their full runtime state (slot_move), so forgetting
     * them would destroy a live copy's history. */
    bt_weapon_rt_forget(s, gone_uid);
    s->wpn_dirty = 1;
    /* after_weapon_removed call order (run_data.gd:1000-1009): unapply the
     * copy's item effects (drop its stat-layer rows and its bucket pairs) ->
     * update_sets -> then update_item_related_effects re-derives from the
     * reduced inventory. */
    bt_weapon_statfx_apply(s, gone, gone_cursed ? gone_fx : 0, -1);
    bt_iwb_unregister_weapon(s, gone, gone_cursed ? gone_fx : 0);
    /* after_weapon_removed -> update_sets (run_data.gd:1005-1009): a set bonus
     * may have lost its quorum. */
    bt_weapon_set_refresh(s);
    bt_item_wbonus_refresh(s);
    return gone;
}

/* Public "remove by index" name (run_data.gd:981). */
BT_HD static int bt_weapon_remove(BT_DEVICE BtState* s, int slot) { return bt_weapon_erase(s, slot); }

/* run_data.gd:1010-1022: unapply everything and clear the array. */
BT_HD static void bt_weapon_remove_all(BT_DEVICE BtState* s) {
    for (int i = (int)s->wpn_count - 1; i >= 0; i--) bt_weapon_erase(s, i);
}

/* Full reset (bt_core_reset hook). Identity arrays + the starting queue + the
 * uid allocator; the runtime arrays are re-zeroed through the hook. */
BT_HD static void bt_weapon_reset(BT_DEVICE BtState* s) {
    for (int i = 0; i < BT_WEAPON_SLOTS; i++) {
        s->wpn_id[i] = BT_WEAPON_NONE;
        s->wpn_tier[i] = 0;
        s->wpn_cursed[i] = 0;
        s->wpn_curse[i] = 0.0f;
        s->wpn_wmod[i] = 0.0f;
        for (int c = 0; c < BT_WEAPON_FX_CURSE_MAX; c++) s->wpn_fxcurse[i][c] = 0.0f;
        s->wpn_cd[i] = 0.0f;
        s->wpn_tracked[i] = 0;
        s->wpn_uid[i] = BT_WEAPON_UID_NONE;
        bt_weapon_rt_erase(s, i);   /* zero the runtime arrays, never a reset draw */
    }
    s->wpn_count = 0;
    s->wpn_overflow = 0;
    s->wpn_dirty = 0;
    s->wpn_uid_next = 0u;   /* safe only because bt_core_reset also clears every queue holding a uid */
    /* Drop the applied-set bookkeeping WITHOUT unapplying it: an unapply here
     * would fold the old tier into stat_raw before the core has re-seeded the
     * stat layers. The sentinel makes the next bt_weapon_set_refresh a pure
     * re-derivation with nothing to undo. */
    for (int i = 0; i < BT_NWEAPON_SETS; i++) s->wpn_set_tier[i] = BT_WEAPON_SET_TIER_NONE;
    for (int i = 0; i < BT_WEAPON_START_MAX; i++) {
        s->wpn_start_id[i] = BT_WEAPON_NONE;
        s->wpn_start_cursed[i] = 0;
    }
    s->wpn_start_count = 0;
    s->wpn_start_overflow = 0;
}

/* ---------------------------------------------------------------------------
 * Eligibility + combination rules.
 * ------------------------------------------------------------------------- */
/* run_data.gd:1262-1280 has_weapon_slot_available. */
BT_HD static int bt_weapon_slot_available(BT_DEVICE BtState* s, unsigned short id) {
    int cap = bt_weapon_slot_cap(s);
    int n = (int)s->wpn_count;
    if (bt_weapon_no_duplicate(s)) {
        unsigned short fam = bt_weapon_family(id);
        for (int i = 0; i < n; i++)
            if (bt_weapon_family(s->wpn_id[i]) == fam) return 0;
    }
    int kind = bt_weapon_kind(id);
    if (kind != BT_WEAPON_KIND_MELEE && kind != BT_WEAPON_KIND_RANGED)
        return n < cap;                              /* WeaponData.type == -1 branch */
    int nb = bt_weapon_kind_copies(s, kind);
    int max_slots = bt_weapon_kind_cap(s, kind);
    int lim = cap < max_slots ? cap : max_slots;
    return n < cap && nb < lim ? 1 : 0;
}

/* run_data.gd:1317-1326 can_combine. */
BT_HD static int bt_weapon_can_combine(BT_DEVICE BtState* s, int slot) {
    if (slot < 0 || slot >= (int)s->wpn_count) return 0;
    unsigned short id = s->wpn_id[slot];
    return bt_weapon_copies_of(s, id) >= 2 &&
           bt_weapon_upgrades(id) != BT_WEAPON_NONE &&
           bt_weapon_tier(id) < bt_weapon_max_tier(s) ? 1 : 0;
}

/* base_shop.gd:650-668 duplicate pick, folded into the core: the first copy of
 * the same canonical id (excluding the source) whose cursed flag DIFFERS from
 * the source's, else the LAST one. The scan is done BEFORE any mutation so an
 * impossible combine changes nothing (no erase, no uid churn, no rand draw);
 * base_shop.gd scans the container after removing the source, which yields the
 * same victim in the same order.
 *
 * base_shop.gd:631-703 _combine_weapon core.
 * Consumes two same-id copies (the source copy, then one duplicate) and inserts
 * upgrades_into, cursed when either input was; the produced copy's curse
 * modifier is a fresh curse_item draw with the merged copies' curse_factor as
 * the floor (dlc_1_data.gd:48-50 early-returns only for a cursed TEMPLATE, and
 * no shipped weapon template is cursed -- 0 of 270 weapon resources carry
 * is_cursed = true). `src_slot` >= 0 marks the source copy as an owned slot
 * (erase it); -1 means the source is the incoming weapon that is not in the
 * array yet (base_shop.gd:602-615 buy path).
 * Returns the new slot, or -1 with wpn_overflow set when no duplicate existed. */
BT_HD static int bt_weapon_combine_core(BT_DEVICE BtState* s, unsigned short id, int cursed_src,
                                        float curse_src, int tracked_src, int src_slot) {
    unsigned short up = bt_weapon_upgrades(id);
    if (!bt_weapon_valid(id) || up == BT_WEAPON_NONE) return -1;
    int pick = -1;
    for (int i = 0; i < (int)s->wpn_count; i++) {
        if (i == src_slot) continue;
        if (s->wpn_id[i] != id) continue;
        pick = i;
        if ((s->wpn_cursed[i] ? 1 : 0) != (cursed_src ? 1 : 0)) break;
    }
    if (pick < 0) { s->wpn_overflow = 1; return -1; }
    int cursed_new = (cursed_src || s->wpn_cursed[pick]) ? 1 : 0;
    float floor_mod = curse_src > s->wpn_curse[pick] ? curse_src : s->wpn_curse[pick];
    int tracked = tracked_src + s->wpn_tracked[pick];
    if (src_slot >= 0) {
        bt_weapon_erase(s, src_slot);
        if (pick > src_slot) pick--;                 /* the erase shifted the tail down */
    }
    bt_weapon_erase(s, pick);
    /* The produced copy is cursed through curse_item(upgrades_into, player,
     * false, floor_mod): the randomized path, with min_modifier = the merged
     * cursed copies' curse_factor (base_shop.gd:669-690). */
    BtWeaponCurse c;
    if (cursed_new) bt_weapon_curse_roll(s, up, 1, 0.0f, floor_mod, &c);
    else bt_weapon_curse_clear(&c);
    return bt_weapon_insert(s, -1, up, tracked, &c);
}

/* Combine the copy in `slot` with another same-id copy. */
BT_HD static int bt_weapon_combine(BT_DEVICE BtState* s, int slot) {
    if (slot < 0 || slot >= (int)s->wpn_count) return -1;
    return bt_weapon_combine_core(s, s->wpn_id[slot], s->wpn_cursed[slot] ? 1 : 0,
                                  s->wpn_curse[slot], s->wpn_tracked[slot], slot);
}

/* base_shop.gd:631-703 with is_upgrade = true (nb_to_remove = 1): the anvil's
 * upgrade_random_weapon path and any single-copy tier bump. The copy keeps its
 * tracked_value; base_shop.gd also copies dmg_dealt_last_wave here, which the
 * core does not model (no consumer). */
BT_HD static int bt_weapon_upgrade(BT_DEVICE BtState* s, int slot) {
    if (slot < 0 || slot >= (int)s->wpn_count) return -1;
    unsigned short id = s->wpn_id[slot];
    unsigned short up = bt_weapon_upgrades(id);
    if (!bt_weapon_valid(id) || up == BT_WEAPON_NONE) return -1;
    int cursed_new = s->wpn_cursed[slot] ? 1 : 0;
    float floor_mod = s->wpn_curse[slot];
    int tracked = s->wpn_tracked[slot];
    bt_weapon_erase(s, slot);
    /* is_upgrade still curses the produced variant through curse_item(..., false,
     * min_factor) with min_factor = the source copy's curse_factor. */
    BtWeaponCurse c;
    if (cursed_new) bt_weapon_curse_roll(s, up, 1, 0.0f, floor_mod, &c);
    else bt_weapon_curse_clear(&c);
    return bt_weapon_insert(s, -1, up, tracked, &c);
}

/* base_shop.gd:602-615 buy_weapon: buy into a free slot when the source
 * eligibility rule passes; otherwise, when an owned same-id copy can still
 * upgrade, consume it and buy the upgraded variant instead (full-slot
 * auto-combination). The source pushes the 7th weapon onto the unbounded Array
 * and then combines; the net mutation is implemented directly (consume the
 * duplicate, insert upgrades_into) so the fixed 6-slot record never needs a
 * temporary 7th slot. Returns the resulting slot, or -1 when the weapon cannot
 * be bought at all (no free slot and no auto-combine; sets wpn_overflow). */
BT_HD static int bt_weapon_buy(BT_DEVICE BtState* s, unsigned short id, int cursed, float curse_mod) {
    if (!bt_weapon_valid(id)) { s->wpn_overflow = 1; return -1; }
    if (bt_weapon_slot_available(s, id)) return bt_weapon_add(s, id, cursed, curse_mod);
    if (bt_weapon_upgrades(id) == BT_WEAPON_NONE) { s->wpn_overflow = 1; return -1; }
    for (int i = 0; i < (int)s->wpn_count; i++) {
        if (s->wpn_id[i] != id) continue;
        return bt_weapon_combine_core(s, id, cursed, curse_mod, 0, -1);
    }
    s->wpn_overflow = 1;
    return -1;
}

/* ---------------------------------------------------------------------------
 * Value + recycle (item_service.gd:701-742).
 * ------------------------------------------------------------------------- */
/* get_endless_factor(wave)/5 (run_data.gd:775-782) -- 0 for waves <= 20. */
BT_HD static float bt_weapon_endless_factor(int wave) {
    int ew = wave - 20; if (ew < 0) ew = 0;
    float mult = 2.0f + (wave > 35 ? (float)(wave - 35) * 0.2f : 0.0f);
    return (((float)(ew * (ew + 1)) / 2.0f) / 100.0f) * mult;
}
/* item_service.gd:701-719 get_value with is_weapon = true, on an explicit base
 * value (`from_value`/`base_value` in the source). `affected` folds in the
 * weapons_price + items_price factors (the recycle path disables them past the
 * campaign's last wave). */
BT_HD static int bt_weapon_value_from_at(BT_DEVICE BtState* s, int wave, float base,
                                          int affected) {
    float vawp = affected ? base * (1.0f + s->scalar_effect[BT_ISTR_WEAPONS_PRICE] * 0.01f) : base;
    float ipf = affected ? 1.0f + s->scalar_effect[BT_ISTR_ITEMS_PRICE] * 0.01f : 1.0f;
    float ef = affected ? bt_weapon_endless_factor(wave) / 5.0f : 0.0f;
    float v = (vawp + (float)wave + vawp * (float)wave * 0.1f) * ipf * (1.0f + ef);
    int r = (int)v;                 /* `as int` truncates */
    return r < 1 ? 1 : r;
}
BT_HD static int bt_weapon_value_from(BT_DEVICE BtState* s, float base, int affected) {
    return bt_weapon_value_from_at(s, (int)s->wave, base, affected);
}
BT_HD static int bt_weapon_value_affected_at(BT_DEVICE BtState* s, int wave,
                                             unsigned short id, int affected) {
    if (!bt_weapon_valid(id)) return 0;
    return bt_weapon_value_from_at(s, wave, (float)BT_WEAPONS[id].value, affected);
}
BT_HD static int bt_weapon_value_affected(BT_DEVICE BtState* s, unsigned short id, int affected) {
    return bt_weapon_value_affected_at(s, (int)s->wave, id, affected);
}
/* Shop list price (base shop price before any discount); the _at form prices an
 * offer at the wave its ShopItem was created with (shop_item.gd:129-140). */
BT_HD static int bt_weapon_price_at(BT_DEVICE BtState* s, int wave, unsigned short id) {
    return bt_weapon_value_affected_at(s, wave, id, 1);
}
BT_HD static int bt_weapon_price(BT_DEVICE BtState* s, unsigned short id) {
    return bt_weapon_value_affected(s, id, 1);
}
/* item_service.gd:732-742 get_recycling_value(wave, from_value, is_weapon=true).
 * `from_value` is the source's parameter, so the caller can fold a
 * specific_items_price factor into the base value first (base_shop.gd:718-720);
 * bt_weapon_recycle_value passes BT_WEAPONS[id].value unchanged. */
BT_HD static int bt_weapon_recycle_value_from(BT_DEVICE BtState* s, unsigned short id, int from_value) {
    if (!bt_weapon_valid(id)) return 0;
    if (from_value == 1) return 1;                    /* base_shop.gd's from_value == 1 shortcut */
    int affected = (s->wave <= BT_WAVES) ? 1 : 0;     /* run_data.gd current_wave <= nb_of_waves */
    float base = (float)from_value;
    int shop_value = bt_weapon_value_from(s, base, 1);
    float rv = (float)bt_weapon_value_from(s, base, affected);
    float f = 0.25f + s->scalar_effect[BT_ISTR_RECYCLING_GAINS] * 0.01f;
    if (f < 0.01f) f = 0.01f;
    if (f > 1.0f) f = 1.0f;
    rv *= f;
    if (rv < 1.0f) rv = 1.0f;
    int rec = (int)rv;
    return shop_value < rec ? shop_value : rec;
}
/* The owned copy's recycle value (gold), item_service.gd:732 with is_weapon=true
 * and the weapon's own base value. Demon's hp_shop never applies here: the
 * caller credits the result to s->materials (base_shop.gd:735 add_gold). */
BT_HD static int bt_weapon_recycle_value(BT_DEVICE BtState* s, int slot) {
    if (slot < 0 || slot >= (int)s->wpn_count) return 0;
    unsigned short id = s->wpn_id[slot];
    if (!bt_weapon_valid(id)) return 0;
    return bt_weapon_recycle_value_from(s, id, (int)BT_WEAPONS[id].value);
}

/* ---------------------------------------------------------------------------
 * Weapon interaction consumers (owner: WeaponInteractionRuntime)
 *
 * The two loadout-wide reactions the source drives from OUTSIDE a weapon: a
 * STAT CHANGE (StatsManager -> Weapon.init_stats(false) -> reset_cooldown) and
 * a GOLD PICKUP (main.gd on_gold_picked_up -> reload_when_pickup_gold).  Both
 * read the identity arrays and write only wpn_cd / pet_timer; neither draws RNG
 * (a `get_next_cooldown` here would consume a start-cooldown draw the source
 * never makes on these paths).
 * ------------------------------------------------------------------------- */

/* Defined in brotato_weapon_effects.h, later in the TU (the same cross-module
 * forward-declare bt_curse_modifier / bt_weapon_set_refresh already use). */
BT_HD static int bt_wfx_weapon_reloads_on_gold(BT_CONST_PTR(unsigned short) rows, int n_rows);
BT_HD static int bt_wfx_reload_all_on_gold(BT_DEVICE BtState* s);
/* Ranged's stats-change entry point (brotato_weapon_ranged.h:114-116 +
 * bt_weapon_ranged_stats_changed) and the kind-agnostic re-clamp helper
 * bt_weapon_reset_cooldown, both defined AFTER this header in the TU: they need
 * the resolved BtPlayerWeaponStats, whose type is declared after this header. */
#if defined(BT_WEAPON_RANGED_HOOKS)
BT_HD static void bt_weapon_ranged_stats_changed(BT_DEVICE BtState* s, int slot);
#endif
BT_HD static void bt_weapon_reset_cooldown(BT_DEVICE BtState* s, int slot);

/* Weapon._is_shooting, held by the kind that owns the cooldown: Melee's
 * wpn_melee_shooting, Ranged's wrng_shoot (both per dense slot). */
BT_HD static int bt_weapon_is_shooting(BT_DEVICE BtState* s, int slot) {
    unsigned short wid = s->wpn_id[slot];
    if (wid == BT_WEAPON_NONE || wid >= BT_NWEAPONS) return 0;
    if (BT_WEAPONS[wid].kind == BT_WEAPON_KIND_MELEE)
        return s->wpn_melee_shooting[slot] ? 1 : 0;
    return s->wrng_shoot[slot] ? 1 : 0;
}

/* TEMPORARY and PERMANENT stat changes that feed a weapon (item acquired,
 * upgrade taken, temp stat expired, class-set tier re-derived): notify every
 * live weapon so its ALREADY-running cooldown is re-clamped by
 * weapon.gd:324-326 reset_cooldown --
 *   min(_current_cooldown, current_stats.cooldown *
 *       (is_big_reload_active() ? additional_cooldown_multiplier : 1))
 * (stats_manager.gd:36-45: every weapon whose stats changed gets
 * Weapon.init_stats(false) -> reset_cooldown).  ZERO RNG on this path: never
 * get_next_cooldown, which would consume a start-cooldown draw the source does
 * not make here.
 *
 * Each KIND's module owns its own clamp (the resolved BtPlayerWeaponStats type
 * is declared after this header), so this is the single notification the core
 * drives: it is called from bt_core_step exactly when `s->wpn_dirty` is set (a
 * stat mutation or a loadout mutation), so a step with no stat change costs one
 * branch and nothing re-resolves per frame. */
BT_HD static void bt_weapon_stats_changed(BT_DEVICE BtState* s) {
    for (int i = 0; i < (int)s->wpn_count; i++) {
        unsigned short wid = s->wpn_id[i];
        if (wid == BT_WEAPON_NONE || wid >= BT_NWEAPONS) continue;
        /* Ranged slots go through Ranged's own entry point (brotato_weapon_ranged.h:
         * 114-116: init_stats(false) -> reset_cooldown, NO draw), which that
         * helper documents; every other kind uses the shared re-clamp. */
#if defined(BT_WEAPON_RANGED_HOOKS)
        if (BT_WEAPONS[wid].kind == BT_WEAPON_KIND_RANGED) {
            bt_weapon_ranged_stats_changed(s, i);
            continue;
        }
#endif
        bt_weapon_reset_cooldown(s, i);
    }
}

/* main.gd:798-834 on_gold_picked_up (once per gold pickup, from the player's
 * ItemAttractArea and from a Lootworm):
 *   - among the weapons carrying reload_when_pickup_gold and NOT _is_shooting,
 *     the one with the LARGEST _current_cooldown has its cooldown zeroed;
 *   - every BuilderTurret carrying the key gets _cooldown = 0;
 *   - when the player-level boolean (buccaneer character effect) is granted,
 *     EVERY weapon's _current_cooldown is zeroed.
 * NAMED DEVIATION: `structure.effects` come from the turret scene's own data
 * resource, which the canonical tables do not carry; the clause is evaluated
 * against the turret's canonical effect row (its own key, then its sub-effect
 * list).  No shipped turret carries reload_when_pickup_gold -- a grep over the
 * whole recovered + DLC tree finds only the buccaneer character effect and the
 * blunderbuss weapon effect -- so the clause is inert for shipped data and
 * correct for a mod that puts the key on a turret row. */
BT_HD static int bt_weapon_structure_gold_reload(BT_CONST_PTR(BtItemEff) E) {
    if (bt_eff_is(E, BT_ISTR_RELOAD_WHEN_PICKUP_GOLD)) return 1;
    int sc = (int)E->sub_count;
    if (sc <= 0 || sc == 65535) return 0;
    for (int k = 0; k < sc; k++) {
        int idx = (int)E->sub_first + k;
        if (idx < 0 || idx >= BT_NITEM_EFF_INDEX) break;
        int row = (int)BT_ITEM_EFF_INDEX[idx];
        if (row < 0 || row >= BT_NITEM_EFF_ROWS) continue;
        if (bt_eff_is(&BT_ITEM_EFF_ROWS[row], BT_ISTR_RELOAD_WHEN_PICKUP_GOLD)) return 1;
    }
    return 0;
}

BT_HD static void bt_weapon_gold_pickup_reload(BT_DEVICE BtState* s) {
    int best = -1;
    float best_cd = 0.0f;
    for (int i = 0; i < (int)s->wpn_count; i++) {
        unsigned short wid = s->wpn_id[i];
        if (wid == BT_WEAPON_NONE || wid >= BT_NWEAPONS) continue;
        int first = (int)BT_WEAPONS[wid].fx_first;
        int cnt = (int)BT_WEAPONS[wid].fx_count;
        if (cnt <= 0 || first < 0 || first + cnt > BT_NWEAPON_EFF_INDEX) continue;
        /* constant-space canonical fx slice (the hitbox view Melee also passes) */
        BT_CONST_PTR(unsigned short) rows = &BT_WEAPON_EFF_INDEX[first];
        if (!bt_wfx_weapon_reloads_on_gold(rows, cnt)) continue;
        if (bt_weapon_is_shooting(s, i)) continue;
        if (best < 0 || s->wpn_cd[i] > best_cd) { best = i; best_cd = s->wpn_cd[i]; }
    }
    if (best >= 0) s->wpn_cd[best] = 0.0f;
    for (int p = 0; p < s->pet_n; p++) {
        if (!s->pet_alive[p]) continue;
        int op = (int)s->pet_opcode[p];
        if (op != BT_ITEMEFF_BUILDER_TURRET && op != BT_ITEMEFF_BUILDER_TURRET_UPGRADE) continue;
        int row = (int)s->pet_row[p];
        if (row < 0 || row >= BT_NITEM_EFF_ROWS) continue;
        if (!bt_weapon_structure_gold_reload(&BT_ITEM_EFF_ROWS[row])) continue;
        s->pet_timer[p] = 0.0f;
    }
    if (bt_wfx_reload_all_on_gold(s))
        for (int i = 0; i < (int)s->wpn_count; i++) s->wpn_cd[i] = 0.0f;
}

/* ---------------------------------------------------------------------------
 * Starting weapons (run_data.gd:1540-1578 add_starting_items_and_weapons).
 *
 * The source adds, for every `[weapon_index, count]` entry in the character's
 * `starting_weapon` KEY_VALUE effect bucket (plus its `cursed_starting_weapon`
 * bucket when the DLC is active), `count` copies via add_weapon -- unconditional
 * of the slot capacity. This module splits that into (1) `enqueue`, which
 * records the resolved canonical ids (the profile/selection resolution belongs
 * to the host + WeaponCharacters), and (2) `apply`, called once per run after
 * the profile is applied. Exceeding BT_WEAPON_START_MAX sets wpn_start_overflow;
 * exceeding the 6-slot bound during apply sets wpn_overflow -- both explicit.
 * ------------------------------------------------------------------------- */
BT_HD static void bt_weapon_starting_enqueue(BT_DEVICE BtState* s, unsigned short id, int cursed) {
    if (!bt_weapon_valid(id)) { s->wpn_start_overflow = 1; return; }
    if ((int)s->wpn_start_count >= BT_WEAPON_START_MAX) { s->wpn_start_overflow = 1; return; }
    s->wpn_start_id[s->wpn_start_count] = id;
    s->wpn_start_cursed[s->wpn_start_count] = cursed ? 1 : 0;
    s->wpn_start_count = (unsigned char)(s->wpn_start_count + 1);
}

/* Append every queued starting weapon in source order and clear the queue.
 *
 * A cursed starting weapon is cursed through
 * `dlc.curse_item(weapon, player_index, true)` (run_data.gd:1572-1578), i.e.
 * turn_randomization_off = true: no draw, and every modifier is
 * BT_WEAPON_CURSE_DET_MOD (bt_weapon_curse_roll's `randomized == 0` path). */
BT_HD static int bt_weapon_starting_apply(BT_DEVICE BtState* s) {
    int added = 0;
    for (int i = 0; i < (int)s->wpn_start_count; i++) {
        unsigned short id = s->wpn_start_id[i];
        int cursed = s->wpn_start_cursed[i] ? 1 : 0;
        if (bt_weapon_add(s, id, cursed, 0.0f) >= 0) added++;
    }
    s->wpn_start_count = 0;
    for (int i = 0; i < BT_WEAPON_START_MAX; i++) {
        s->wpn_start_id[i] = BT_WEAPON_NONE;
        s->wpn_start_cursed[i] = 0;
    }
    return added;
}
