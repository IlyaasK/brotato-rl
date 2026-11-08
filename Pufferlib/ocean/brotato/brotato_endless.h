/* brotato_endless.h -- endless-mode mechanics (waves past BT_WAVES).
 *
 * SOURCE PROVENANCE (brotato_recovered v1.1.14.6 + brotato_abyss DLC)
 * ---------------------------------------------------------------------------
 *   run_data.gd:196-197        is_endless_run (the run-options toggle,
 *                              ProgressData.settings.endless_mode_toggled,
 *                              endless_button.gd:6-14).  The core carries it as
 *                              BtState.endless and, when set, never ends the run
 *                              at the wave-20 clear.
 *   run_data.gd:775-783        get_endless_factor(wave):
 *                                  endless_wave = max(0, wave - 20)
 *                                  endless_mult = 2.0 + max(0, (wave - 35)*0.2)
 *                                  E = max(0, (endless_wave*(endless_wave+1))/2 / 100)
 *                                      * endless_mult
 *                              (integer inner division; byte-identical to
 *                              bt_loot_endless_factor in brotato_loot.h).
 *   entity_service.gd:11-53    enemy health/damage final-stat factors:
 *                                  health: max(0.01, 1 + E*2.25 * (1 + curse))
 *                                  damage: max(0.01, 1 + E     * (1 + curse))
 *                              (single player: accessibility/coop = 1, no curse).
 *   entity_service.gd:55-66    enemy speed factor:
 *                                  effects * accessibility * (1 + min(1.75, E/13.33))
 *                              the min(1.75, ...) cap is reached at wave 51
 *                              (E(50)=23.25 < 23.33 <= E(51)=25.79), matching the
 *                              wiki's "enemy speed stops scaling at wave 51".
 *   enemy.gd:276,290,302       take_damage's percent-of-current-health bonuses
 *                              (bonus_current_health_damage, giant_crit,
 *                              burning_enemy_hp_percent_damage) divide their
 *                              bonus by max(1, E*0.2) -- the Giant Belt /
 *                              Greek Fire endless nerf.
 *   player.gd:565-573          get_iframes divides MIN/MAX_IFRAMES by
 *                              max(1, E) (implemented at bt_iframes in core).
 *   run_data.gd:785-788        get_additional_elites_endless:
 *                                  ceil((wave - 20)/10) extra zone elites.
 *   run_data.gd:603-621        get_bosses_to_spawn(true): 2 distinct zone bosses
 *                              sampled without replacement (fresh draws).
 *   run_data.gd:629-679        init_elites_spawn(base_wave, horde_chance); the
 *                              clean_up_room reschedule calls it with base =
 *                              current_wave + 10 and horde_chance 0.0 for
 *                              current_wave % 10 == 0 and >= 10 (main.gd:931-933).
 *   item_service.gd:718-724    shop/reroll endless price terms (already in core
 *                              bt_shop_price / bt_reroll_cost).
 *   main.gd:1464-1492          endless harvesting decay (AuditProgression's
 *                              bt_progress_end_wave).
 *
 * The endless OWN-WAVE composition (zone_service.gd:36-88 get_wave_data /
 * get_additional_groups + the entity_spawner.gd:151-160 max_enemies cull and the
 * BT_MAX bound) is owned by the AuditWaves slice; this header only provides the
 * run-level scaling the wave data is then priced/stat-scaled by.
 */
#pragma once

#include "brotato_state.h"
#include "brotato_waves.h"

/* run_data.gd:775-783 get_endless_factor, defined in brotato_loot.h (included
 * by brotato_core.h before this header).  Declared here so this header is
 * self-contained. */
BT_HD static float bt_loot_endless_factor(int wave);

/* E(wave) == get_endless_factor(wave).  Named alias for the run-level factor the
 * endless consumers below share. */
BT_HD static float bt_endless_factor(int wave) {
    return bt_loot_endless_factor(wave);
}

/* entity_service.gd:14-31 get_final_enemy_health's endless factor
 * (single player, no curse): max(0.01, 1 + E*2.25).  Exactly 1.0 for waves <= 20
 * (E == 0), so the campaign numbers are unchanged bit-for-bit. */
BT_HD static float bt_endless_enemy_health_mult(int wave) {
    float f = 1.0f + bt_loot_endless_factor(wave) * 2.25f;
    return f > 0.01f ? f : 0.01f;
}

/* entity_service.gd:35-53 get_final_enemy_damage's endless factor:
 * max(0.01, 1 + E). */
BT_HD static float bt_endless_enemy_damage_mult(int wave) {
    float f = 1.0f + bt_loot_endless_factor(wave);
    return f > 0.01f ? f : 0.01f;
}

/* entity_service.gd:55-66 get_final_enemy_speed's endless factor:
 * 1 + min(1.75, E/13.33).  The cap binds from wave 51 onward. */
BT_HD static float bt_endless_enemy_speed_mult(int wave) {
    float e = bt_loot_endless_factor(wave) / 13.33f;
    if (e > 1.75f) e = 1.75f;
    return 1.0f + e;
}

/* enemy.gd:276,290,302: the divisor for a percent-of-current-health bonus:
 * max(1.0, E * 0.2).  This is the Giant Belt nerf (integer division of the
 * bonus by this factor), and the same divisor the burning Greek Fire bonus and
 * bonus_current_health_damage use. */
BT_HD static float bt_endless_hp_pct_divisor(int wave) {
    float d = bt_loot_endless_factor(wave) * 0.2f;
    return d > 1.0f ? d : 1.0f;
}

/* run_data.gd:785-788 get_additional_elites_endless: ceil((wave - 20)/10), 0 for
 * waves <= 20. */
BT_HD static int bt_endless_extra_elites(int wave) {
    int n = wave - 20;
    return n > 0 ? (n + 9) / 10 : 0;
}

/* zone_service.gd:36-52 get_wave_data's base-resource index for index > 20:
 *   wave_index = (index - 1) % 20; if wave_index < 10: wave_index += 10
 * (the wave resource reused for an endless wave).  Owned by AuditWaves for the
 * group list; exposed here for the boss-group test below. */
BT_HD static int bt_endless_base_wave_index(int wave) {
    int wi = (wave - 1) % BT_WAVES;
    if (wi < 10) wi += 10;
    return wi;
}

/* wave_manager.gd:76-90: the endless bosses + the extra endless-elite group are
 * appended only when the reused wave resource carries an is_boss group.  Only
 * wave 20's resource (zone_1/020/group_1.tres, BT_GROUPS[110]) has one, i.e.
 * base index 19 -- reached by endless waves 30, 40, 50, ... (wave % 10 == 0 with
 * wave >= 30, equivalently (wave - 1) % 20 in {9, 19}). */
BT_HD static int bt_endless_wave_has_boss_group(int wave) {
    return wave > BT_WAVES && bt_endless_base_wave_index(wave) == 19;
}
