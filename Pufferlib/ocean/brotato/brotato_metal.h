#ifndef PUFFER_BROTATO_METAL_H
#define PUFFER_BROTATO_METAL_H
#import <Metal/Metal.h>
#include "brotato_state.h"

/* Metal GPU backend for the shared Brotato core.
 *
 * There is no simulation code here: the Metal kernel drives the SAME
 * `bt_core_init/reset/step/observe/add_log` functions from brotato_core.h that
 * the CPU env calls, on the same POD `BtState`, so all three backends share one
 * numerical definition. The kernel source composes brotato_core.h (and its
 * include closure) into the MSL translation unit at library-compile time; the
 * file that defines the core is the only definition of the sim.
 *
 * Layout contract:
 *   observations  float32 [count * BT_OBS_SIZE]   written by the kernel
 *   actions       float32 [count]                 written by the caller
 *   rewards       float32 [count]                 written by the kernel
 *   terminals     float32 [count]                 written by the kernel
 * All four buffers belong to the caller and must outlive the batch. Per-env
 * simulation state lives in a buffer owned by the handle.
 *
 * Env i is initialised once, at create, with the per-env seed derived from the
 * batch base (see BrotatoMetalConfig), the caller's zone/shop length and the
 * caller's resolved character/starting-weapon ids; reset does not re-seed,
 * matching the CPU env's one-shot RNG seeding across episodes.
 * `brotato_metal_reset` also does NOT clear the numeric log, so `metal_env_log`
 * can accumulate episodes across rollouts exactly like the CPU and CUDA paths.
 *
 * Operations are synchronous: they enqueue on a private command queue and wait
 * for completion before returning, so the caller may setBytes/memcpy the shared
 * buffers immediately afterwards. A caller producing buffers on another queue
 * must synchronise that queue first.
 */

#define BROTATO_METAL_OBS_SIZE BT_OBS_SIZE

/* Layout is mirrored byte-for-byte by the MSL `BrotatoMetalConfig`; the seed
 * leads so both compilers place the 64-bit field without padding surprises
 * (host sizeof == 32, MSL sizeof == 32: u64 + five ints, padded to the 8-byte
 * alignment the u64 imposes).
 *
 * `character` / `starting_weapon` are canonical ids already resolved on the
 * host from the config strings (bt_character_id_from_name /
 * bt_weapon_id_from_name): BT_CHARACTER_BULL = 0 and BT_WEAPON_NONE = 0xFFFF
 * reproduce the default weaponless Bull, and both mirrors place them the same way.
 *
 * `seed` is the per-batch base. Env `i` is initialised with
 *     raw  = seed + i
 *     seed_i = (raw == 0) ? (1 + i) : raw
 * which is exactly the CPU env's seed input (the harness writes
 * `Env.rng = index` and `puf_init` applies `if (rng == 0) rng = 1 + tag`).
 * So `seed = 0` reproduces the CPU trainer's default per-env seeds
 * [1, 1, 2, 3, ...], and any non-zero `seed` selects a distinct
 * `seed + index` stream. */
struct BrotatoMetalConfig {
    unsigned long long seed;   /* env i uses seed + i (0 -> CPU default) */
    int zone;                  /* 0 = campaign, 1 = abyss; clamped by the core */
    int shop_steps;            /* frames the shop stays open; clamped to >= 1  */
    int character;             /* canonical character id; BT_CHARACTER_BULL = 0 */
    int starting_weapon;       /* canonical weapon id; BT_WEAPON_NONE = 0xFFFF */
    int endless;               /* 0/1: continue past wave 20 (RunData.is_endless_run) */
    int audio_rng;             /* 0/1: 1 = source audio RNG draws (legacy), 0 = bridge-neutralized */
    int particles_rng;         /* 0/1: 1 = source CPUParticles2D draws (legacy), 0 = bridge-suppressed */
    int invulnerable;          /* 0/1: 1 = player Hurtbox disabled (DebugService.invulnerable) */
};

struct BrotatoMetalLog { float perf, score, episode_return, episode_length, n; };

/* Reduced snapshot for rendering and for parity tests. */
struct BrotatoMetalState {
    struct BrotatoMetalLog log;
    float hp, max_hp, px, py, iframe, time_left, wave_duration, episode_return, dmg_taken;
    float materials, xp, arm_max_hp;
    int phase, wave, kills, level;
    float xp_need;
    int ecount, ticks, tick_in_wave, waves_cleared, done, won;
    /* Canonical shop identity: shop_kind[i] (BT_SHOP_NONE/ITEM/WEAPON) +
     * shop_id[i] (BT_ITEMS row for ITEM, BT_WEAPONS row for WEAPON, 0 for NONE)
     * and the per-offer curse flag. lvl_offers[] stays the (BT_UPGRADES row + 1)
     * encoding (0 = no offer, BT_UPGRADE_SLOT_OFFER = the weapon-slot offer). */
    unsigned char shop_kind[4], shop_cursed[4];
    unsigned short shop_id[4], lvl_offers[4];
    /* Profile + owned weapons. FINAL canonical layout (parent-frozen): profile is
     * `character` (unsigned short) + weapon_slot_limit; the dense slot record is
     * wpn_id[]/wpn_tier[]/wpn_cursed[]/wpn_curse[]/wpn_tracked[] with
     * wpn_count/wpn_overflow/wpn_dirty, and the shared runtime cooldown is
     * wpn_cd[] in TICKS; the melee fields are brotato_weapon_melee.h's
     * wpn_melee_*. */
    unsigned short character;
    unsigned char weapon_slot_limit;
    unsigned short starting_weapon;
    unsigned short wpn_id[BT_SLOTS];
    unsigned char wpn_tier[BT_SLOTS], wpn_cursed[BT_SLOTS];
    float wpn_curse[BT_SLOTS];
    int wpn_tracked[BT_SLOTS];
    unsigned int wpn_uid[BT_SLOTS], wpn_uid_next;
    unsigned char wpn_count, wpn_overflow, wpn_dirty;
    float wpn_cd[BT_SLOTS];
    unsigned char wpn_melee_anim[BT_SLOTS], wpn_melee_phase[BT_SLOTS], wpn_melee_hit_on[BT_SLOTS];
    unsigned char wpn_melee_shooting[BT_SLOTS];
    float wpn_melee_rot[BT_SLOTS];
    unsigned long long rng_state;
    int rng_ready;
};

struct BrotatoMetal;

/* Throws std::invalid_argument on a null device / non-positive count /
 * undersized or foreign buffers / unusable configuration, and
 * std::runtime_error if the MSL library or pipeline fails to build (the
 * message carries the Metal compiler diagnostics). */
struct BrotatoMetal *brotato_metal_create(id<MTLDevice> device, int count,
    struct BrotatoMetalConfig config, id<MTLBuffer> observations,
    id<MTLBuffer> actions, id<MTLBuffer> rewards, id<MTLBuffer> terminals);

/* Full-batch episode reset (log preserved). Writes observations, zeroes
 * rewards/terminals. */
void brotato_metal_reset(struct BrotatoMetal *batch);

/* Steps [start, start+count); the rest of the batch is untouched. */
void brotato_metal_step(struct BrotatoMetal *batch, int start, int count);

/* Copy one environment's reduced state. */
struct BrotatoMetalState brotato_metal_state(struct BrotatoMetal *batch, int index);

/* Sum per-env logs, optionally clearing them. */
struct BrotatoMetalLog brotato_metal_log(struct BrotatoMetal *batch, bool clear);

void brotato_metal_close(struct BrotatoMetal *batch);

#endif
