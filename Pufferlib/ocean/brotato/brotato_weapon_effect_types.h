/* brotato_weapon_effect_types.h -- POD types + constants shared by the weapon
 * effect consumers and BtState.
 *
 * OWNERSHIP: WeaponEffects slice. This file holds ONLY plain data: the macros and
 * the structs. It exists so brotato_state.h can embed `BtWeaponFxState` BEFORE
 * brotato_weapon_effects.h is parsed (the consumers need the struct, the state
 * needs the struct, and neither may depend on the other's include order).
 *
 * It includes only the portable qualifier prelude (brotato_rng.h: BT_HD /
 * BT_DEVICE / BT_THREAD / BT_CONST_PTR) and declares no function. Pointer members
 * carry the required explicit address space and are always declared through
 * BT_CONST_PTR -- MSL rejects a pointer member without an address-space
 * qualifier (proven with the real Metal compiler), and BT_CONST_PTR is exactly
 * that qualifier (`constant T*` under MSL, `const T*` on host/CUDA).
 */
#pragma once

#include "brotato_rng.h"

/* weapon.gd:24 MAX_ATTACK_COUNT_HISTORY -- the per-attack tally window. */
#define BT_WFX_ATTACK_HISTORY 20

/* player.tscn:1381 LifestealTimer wait_time = 0.1 s (one_shot).
 * The shared countdown is kept in SECONDS and advanced once per step. */
#define BT_WFX_LIFESTEAL_SECONDS 0.1f

/* Weapon slots. brotato_state.h defines BT_SLOTS; when this header is parsed
 * before it (the state-embed case) the fallback is the shared contract's six. */
#if defined(BT_SLOTS)
#define BT_WFX_SLOTS BT_SLOTS
#else
#define BT_WFX_SLOTS 6
#endif

/* One per-weapon attack tally: weapon.gd keeps `_hit_count_by_attack_id` /
 * `_kill_count_by_attack_id` (a 20-deep window) plus `_oldest_attack_id` ON EACH
 * Weapon node, i.e. MAX_ATTACK_COUNT_HISTORY is PER WEAPON, not shared. The
 * window is scoped by the owning copy's uid, so it survives slot compaction and
 * two weapons can never evict each other's history. */
typedef struct {
    int            attack[BT_WFX_ATTACK_HISTORY];
    unsigned short n[BT_WFX_ATTACK_HISTORY];
    unsigned char  used[BT_WFX_ATTACK_HISTORY];
    int            oldest;                     /* weapon.gd _oldest_attack_id */
} BtWeaponAttackTally;

/* One weapon copy's live history: its uid (BT_WFX_UID_FREE when the block is
 * unused) and the two per-attack windows. */
typedef struct {
    unsigned int        uid;
    BtWeaponAttackTally hit;
    BtWeaponAttackTally kill;
} BtWeaponAttackLog;

/* UID sentinel shared with WeaponInventory (same value; the guarded define keeps
 * the two headers agreeing whichever is parsed first). 0 IS a valid uid. */
#ifndef BT_WEAPON_UID_NONE
#define BT_WEAPON_UID_NONE 0xFFFFFFFFu
#endif
#define BT_WFX_UID_FREE BT_WEAPON_UID_NONE

/* Per-run / per-wave weapon event state: ONE full 20-entry history per weapon
 * copy (BT_WFX_SLOTS x 20, matching the source's six Weapon nodes), plus the
 * per-slot `_enemies_killed_this_wave_count`.
 *   log[i]        copy i's (uid, hit, kill) history; the block moves with its
 *                 copy through Inventory densification (bt_wfx_slot_move) and is
 *                 cleared by bt_wfx_slot_erase / bt_wfx_forget_uid.
 *   wave_kills[i] the slot's wave kill count.
 *   overflow      set when a uid's history could not be claimed (never a silent
 *                 oldest overwrite across owners). */
typedef struct {
    BtWeaponAttackLog log[BT_WFX_SLOTS];
    unsigned short    wave_kills[BT_WFX_SLOTS];
    unsigned char     overflow;
} BtWeaponFxState;

/* One enemy hit's weapon context, handed to the shared enemy-damage primitive
 * (bt_item_expl_hit_enemy) so a weapon hitbox's own effect rows reach the SINGLE
 * damage formula. `rows` is a contiguous canonical BT_ITEM_EFF_ROWS id array (the
 * weapon's fx_first/fx_count slice through BT_WEAPON_EFF_INDEX); pass count 0
 * when the hit carries no weapon rows.
 *   is_elemental            resolved by WeaponStats (BtPlayerWeaponStats.is_elemental)
 *                           from the weapon's EFFECTIVE scaling rows at
 *                           weapon_service.gd:109-112. Never derive it from
 *                           bt_wfx_item_scaling_is_elemental, which reads the
 *                           ITEM scaling table (a different namespace).
 *   one_shot                bt_wfx_one_shot() result (hit deals >= current HP)
 *   current_health_damage   bt_wfx_current_health_damage() (post-armor, flat)
 *   owner_uid / attack_id   the firing weapon copy + attack, for the tallies
 *   was_crit                OUT: 1 when the primitive's crit roll succeeded, so
 *                           the caller can run the crit events. */
typedef struct {
    BT_CONST_PTR(unsigned short) rows;
    int          count;
    int          is_elemental;
    int          one_shot;
    float        current_health_damage;
    unsigned int owner_uid;
    int          attack_id;
    int          was_crit;
} BtEnemyHitContext;
