/* brotato_effects.h -- item/status effect mechanics kept out of the core.
 *
 * Owner: SourceGameplay (core/state slice). Every rule is transcribed from the
 * recovered Brotato v1.1.14.6 scripts with file:line provenance:
 *   run_data.gd:1305-1370   get_stat/gain, get_armor_coef, hp-regen timer
 *   run_data.gd:1853-1890   gain_stat_for_every_stat_effect (stat links)
 *   player_run_data.gd      init_stats caps (hp LARGE, speed LARGE,
 *                           dodge_cap 60, crit_chance_cap LARGE, ...)
 *   utils.gd:469-484        get_capped_stat
 *   enemy.gd / unit.gd      burning + knockback status hooks
 *
 * POD + BT_HD only, no allocation, no host API -> CPU/CUDA/MSL portable.
 */
#pragma once

#include "brotato_rng.h"

/* Utils.LARGE_NUMBER is the "no cap" sentinel (init_stats: hp_cap, speed_cap,
 * crit_chance_cap). dodge_cap is 60 (%). */
#define BT_STAT_CAP_NONE 1000000000.0f
#define BT_DODGE_CAP_DEFAULT 60.0f

/* utils.gd:469-484 get_capped_stat: stat caps come from the *_cap effect, which
 * item effects (handcuffs/shackles stat_cap) can lower; only dodge has a real
 * finite default (60). The five source upgrade stats 14..18 (accuracy, attack
 * speed, engineering, lifesteal, range) have no *_cap entry in
 * player_run_data.gd:387-443 init_stats, so they fall through to "no cap". */
BT_HD static inline float bt_stat_cap(int stat, float item_cap) {
    switch (stat) {
    case 4:  return item_cap > 0.0f ? item_cap : BT_DODGE_CAP_DEFAULT;  /* dodge  */
    case 0:  return item_cap > 0.0f ? item_cap : BT_STAT_CAP_NONE;      /* max hp */
    case 3:  return item_cap > 0.0f ? item_cap : BT_STAT_CAP_NONE;      /* speed  */
    case 9:  return item_cap > 0.0f ? item_cap : BT_STAT_CAP_NONE;      /* crit   */
    default: return item_cap > 0.0f ? item_cap : BT_STAT_CAP_NONE;      /* 1,2,5..8,10..18 */
    }
}

BT_HD static inline float bt_cap_apply(float value, float cap) {
    return value > cap ? cap : value;
}

/* run_data.gd:1893 get_scaling_bonus: int(value * (actual_stat / nb_stat_scaled)).
 * nb_stat_scaled is a real SIGNED denominator (Esty'sCouch uses -1, "regen per
 * negative speed"): with speed=-10 and nb=-1 this is int(2*10)=+20, not -20.
 * Only nb == 0 is invalid (no divide); there is no abs()/sentinel clamping. */
BT_HD static inline float bt_stat_link_gain(float value, float src, float nb) {
    if (nb == 0.0f) return 0.0f;
    return (float)(int)(value * (src / nb));
}
