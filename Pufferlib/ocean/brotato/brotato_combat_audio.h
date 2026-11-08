/* brotato_combat_audio.h -- the gameplay-RNG half of the NON-weapon combat
 * audio events.  Rendering is omitted; the selections and the queue admissions
 * the source performs are not, because every `Utils.get_rand_element` and every
 * `SoundManager(2D).play` pitch draw consumes the SAME global RNG the rest of
 * the simulation draws from.
 *
 * OWNERSHIP.  This header + brotato_item_explosions.h / brotato_scalar_events.h /
 * brotato_progression.h (their audio consumers) are CombatAudioRuntime's.  It
 * NEVER re-implements another owner's audio:
 *   - a weapon shot / custom hit  -> brotato_weapon_audio.h (WeaponAudioRuntime)
 *   - footsteps                   -> brotato_player_visual.h (PlayerVisual)
 *   - entity births + healer heal -> brotato_birth_audio.h (parent)
 *   - the two queues themselves   -> brotato_state.h main_audio_* / birth_audio_*
 *     The healer is the one place the two overlap, and they stay separate: the
 *     healer's OWN `heal_sound` submission is the parent's
 *     (bt_birth_audio_enqueue(BT_POSITIONAL_AUDIO_HEAL) at enemy_lifecycle.h), while
 *     its `body.on_healing_effect(heal_value)` on a damaged PLAYER is the player
 *     heal funnel, so the hp_regen pick is taken by bt_scalar_heal below.
 * For the same reason the clip layout lives in the generated brotato_audio_data.h
 * (extract_audio.py, CombatAudioData) and only the CALL-SITE properties (queue,
 * pitch_rand, always_play, draw-or-direct) live here: they are code, not data.
 *
 * SOURCE, unless stated, is /Users/leelachebiyyam/brotato_recovered:
 *   entities/units/unit/unit.gd:282-323   take_damage sound selection + play
 *   entities/units/unit/unit.gd:642       apply_burning's new-burn sound
 *   entities/units/unit/unit.gd:661-707   BurningTimer tick -> take_damage
 *   entities/units/unit/unit.gd:749-751   _on_one_shotted
 *   entities/units/player/player.gd:395-408 on_weapon_broken
 *   entities/units/player/player.gd:436-507 take_damage hurt/dodge sound
 *   entities/units/player/player.gd:544-560 explode() shooting_sounds
 *   entities/units/player/player.gd:730-745 on_healing_effect
 *   entities/units/player/player.gd:850-875 alien eyes
 *   main.gd:455 run lost, 798-804 gold pickup, 863 level up, 958 wave clear
 *            (clean_up_room), 1030 run won
 *   singletons/weapon_service.gd:9-13     breaking_sounds
 *   singletons/weapon_service.gd:339-352  manage_special_spawn_projectile
 *   singletons/utils.gd:391-394           get_rand_element -> Array.pick_random
 *   singletons/sound_manager.gd:7-14      main SoundManager admission
 *   singletons/sound_manager_2d.gd:7-14   SoundManager2D admission
 *   singletons/sound_manager_parent.gd:39-48  ONE clip per idle frame + pitch
 *
 * DRAW RULES (the whole point):
 *   * `Utils.get_rand_element(array)` == `Array.pick_random()` ==
 *     `array[Math::rand() % size]`: ONE bounded global draw, and it is SKIPPED
 *     entirely for an empty array (`utils.gd:392` returns null first).  A size-1
 *     array still takes its one draw -- EXCEPT the two DIRECT events
 *     (BT_AUDIO_EV_LEVEL_UP, BT_AUDIO_EV_ONE_SHOT), where the source plays a
 *     literal exported Resource and takes no draw at all.  brotato_audio_data.h
 *     marks that split by event.
 *   * A DIRECT `play(resource, ...)` (a literal exported Resource, not a pick)
 *     takes NO selection draw.
 *   * `SoundManager(2D).play` takes no draw itself: the `1.0 + rand_range(
 *     -pitch_rand, pitch_rand)` pitch draw happens at PLAYBACK in
 *     SoundManagerParent._process, one clip per frame while a player is idle, and
 *     the clip then plays for `stream_length / pitch_scale` seconds.  That is
 *     brotato_weapon_audio.h / brotato_birth_audio.h's bt_*_audio_step; this
 *     header only submits, and it submits the draw the source took EVEN WHEN the
 *     submission is later rejected (`play` is called unconditionally).
 *   * `SoundManager2D.play(sound, pos, db, pitch, true)` / `SoundManager.play(..,
 *     .., .., true)` = always_play: admitted even with every player busy, by
 *     REPLACING the oldest QUEUED entry (Array.pop_front) -- never a playing
 *     channel (bt_*_audio_enqueue_force).
 *
 * POD + BT_HD only, no allocation -> CPU/CUDA/MSL portable.
 */
#pragma once

#include "brotato_state.h"
#include "brotato_audio_data.h"
/* the shared queues: bt_main_audio_enqueue[_force] (SoundManager, 12 players /
 * 16 FIFO) and bt_positional_audio_enqueue[_force] (SoundManager2D, same shape).
 * Both are POD/queue-only headers over brotato_state.h, so this module compiles
 * against state + the generated audio table, never brotato_core.h. */
#include "brotato_weapon_audio.h"

/* pitch_rand 0.0: the source still calls `1.0 + rand_range(-0.0, 0.0)` at
 * playback, so the step functions take a zero magnitude rather than skipping. */
#define BT_COMBAT_PITCH_00 ((bt_u64)0)

/* ---------------------------------------------------------------------------
 * Internal submission helpers.  `first`/`count` is a span of the generated
 * BT_AUDIO_CLIP_SECONDS; `count == 0` means the source array is empty, so the
 * pick is skipped (the helper returns 0.0f) -- the caller decides whether a
 * direct play of a null stream still needs a submission.
 * ------------------------------------------------------------------------- */
BT_HD static float bt_combat_audio_clip(BT_DEVICE BtState* s, unsigned short first,
                                        unsigned short count) {
    if (count == 0u) return 0.0f;    /* empty array: utils.gd:392 returns null first */
    unsigned int idx = bt_rng_bounded(&s->rng, (unsigned int)count);
    return BT_AUDIO_CLIP_SECONDS[(unsigned int)first + idx];
}

BT_HD static void bt_combat_audio_main(BT_DEVICE BtState* s, float duration, bt_u64 pitch_bits,
                                       int force) {
    if (force) bt_main_audio_enqueue_force(s, duration, pitch_bits);
    else bt_main_audio_enqueue(s, duration, pitch_bits);
}

BT_HD static void bt_combat_audio_positional(BT_DEVICE BtState* s, float duration,
                                             bt_u64 pitch_bits, int force) {
    if (force) bt_positional_audio_enqueue_force(s, duration, pitch_bits);
    else bt_positional_audio_enqueue(s, duration, pitch_bits);
}

/* ===========================================================================
 * Enemy units (unit.gd) -- reached by EVERY weapon hitbox hit.
 * ========================================================================= */

/* unit.gd:282-323 take_damage's sound selection, in the source's own order:
 *
 *     var is_crit = Utils.get_chance_success(crit_chance)   # rolled by the caller
 *     ... OneShotOnHitEffect loop (one-shot sound, see below) ...
 *     var sound = Utils.get_rand_element(hurt_sounds)
 *     if is_crit:
 *         sound = Utils.get_rand_element(crit_sounds)
 *     ...
 *     if args.custom_sound: sound = args.custom_sound
 *     SoundManager2D.play(sound, global_position, 0, 0.2)
 *
 * So the hurt pick happens ALWAYS, the crit pick additionally when the hit
 * crits (and REPLACES the hurt clip), and the play is a NORMAL (not always_play)
 * SoundManager2D submission with pitch_rand 0.2.  `args.custom_sound` is only
 * ever set by the burn tick (brotato_combat_audio_enemy_burn_tick), which does
 * not go through this helper.  `kind` is BtState.ekind with `neutral` ==
 * BtState.eneutral: enemy slots index BT_KINDS (BT_AUDIO_KIND_*), neutral slots
 * index BT_NEUTRALS (BT_AUDIO_NEUTRAL_*) -- the same split bt_kunit uses, so a
 * scene that overrides its own sounds uses its own span.
 *
 * Call it from bt_item_expl_hit_enemy AFTER the charmed early-return (a charmed
 * unit is never hittable) and BEFORE the damage math, i.e. exactly where the
 * source is between the crit/one-shot rolls and get_damage_value. */
BT_HD static void bt_combat_audio_enemy_hit(BT_DEVICE BtState* s, int kind, int neutral,
                                            int is_crit) {
    unsigned short hf, hc, cf, cc;
    if (neutral) {
        if (kind < 0 || kind >= BT_NAUDIO_NEUTRAL) kind = 0;
        hf = BT_AUDIO_NEUTRAL_HURT_FIRST[kind]; hc = BT_AUDIO_NEUTRAL_HURT_COUNT[kind];
        cf = BT_AUDIO_NEUTRAL_CRIT_FIRST[kind]; cc = BT_AUDIO_NEUTRAL_CRIT_COUNT[kind];
    } else {
        if (kind < 0 || kind >= BT_NAUDIO_KIND) kind = 0;
        hf = BT_AUDIO_KIND_HURT_FIRST[kind]; hc = BT_AUDIO_KIND_HURT_COUNT[kind];
        cf = BT_AUDIO_KIND_CRIT_FIRST[kind]; cc = BT_AUDIO_KIND_CRIT_COUNT[kind];
    }
    float dur = bt_combat_audio_clip(s, hf, hc);
    if (is_crit) {
        /* `sound = Utils.get_rand_element(crit_sounds)` REPLACES the hurt clip
         * unconditionally: with an empty crit array the source queues a null
         * stream, not the hurt clip. */
        unsigned int ci = (cc != 0u) ? bt_rng_bounded(&s->rng, (unsigned int)cc) : 0u;
        dur = (cc != 0u) ? BT_AUDIO_CLIP_SECONDS[(unsigned int)cf + ci] : 0.0f;
    }
    bt_combat_audio_positional(s, dur, BT_AUDIO_PITCH_RANGE_02, 0);
}

/* unit.gd:618-646 apply_burning, the `else` branch (no burning was active):
 *
 *     SoundManager2D.play(Utils.get_rand_element(burn_sounds), global_position, 0, 0.2)
 *
 * ONE pick then a normal positional submission, pitch_rand 0.2.  The merge
 * branch (`_burning != null`) plays nothing and draws nothing.
 *
 * Call it from bt_apply_enemy_burn in the branch where `s->eburn[j]` was 0. */
BT_HD static void bt_combat_audio_enemy_burn_apply(BT_DEVICE BtState* s, int kind, int neutral) {
    unsigned short bf, bc;
    if (neutral) {
        if (kind < 0 || kind >= BT_NAUDIO_NEUTRAL) kind = 0;
        bf = BT_AUDIO_NEUTRAL_BURN_FIRST[kind]; bc = BT_AUDIO_NEUTRAL_BURN_COUNT[kind];
    } else {
        if (kind < 0 || kind >= BT_NAUDIO_KIND) kind = 0;
        bf = BT_AUDIO_KIND_BURN_FIRST[kind]; bc = BT_AUDIO_KIND_BURN_COUNT[kind];
    }
    bt_combat_audio_positional(s, bt_combat_audio_clip(s, bf, bc), BT_AUDIO_PITCH_RANGE_02, 0);
}

/* unit.gd:661-707 _on_BurningTimer_timeout -> take_damage(burning.damage, args):
 *
 *     args.custom_sound = Utils.get_rand_element(burn_sounds)     # :672, draw 1
 *     ... take_damage:
 *           is_crit = Utils.get_chance_success(crit_chance)       # crit_chance is
 *             0.0 (args.hitbox is null) and get_chance_success(0.0) returns false
 *             WITHOUT a draw (utils.gd:397-401) -> no draw, no crit pick
 *           sound = Utils.get_rand_element(hurt_sounds)           # :298, draw 2
 *           if args.custom_sound: sound = args.custom_sound       # :320-321
 *           SoundManager2D.play(sound, global_position, 0, 0.2)   # :323
 *
 * TWO picks in the order burn-then-hurt, and the BURN clip is what gets queued
 * (the custom_sound override); normal positional submission, pitch_rand 0.2.
 *
 * NOTE for the integrator: the core's burn tick (brotato_core.h, the `burning on
 * hit` block) applies `ehp -= bt_enemy_incoming_damage(...)` directly instead of
 * routing through the enemy-hit primitive, so these two picks are the ONLY RNG
 * the source's burn tick takes (the crit roll is provably draw-free, see above).
 * Calling this helper there is therefore exact, not partial. */
BT_HD static void bt_combat_audio_enemy_burn_tick(BT_DEVICE BtState* s, int kind, int neutral) {
    unsigned short bf, bc, hf, hc;
    if (neutral) {
        if (kind < 0 || kind >= BT_NAUDIO_NEUTRAL) kind = 0;
        bf = BT_AUDIO_NEUTRAL_BURN_FIRST[kind]; bc = BT_AUDIO_NEUTRAL_BURN_COUNT[kind];
        hf = BT_AUDIO_NEUTRAL_HURT_FIRST[kind]; hc = BT_AUDIO_NEUTRAL_HURT_COUNT[kind];
    } else {
        if (kind < 0 || kind >= BT_NAUDIO_KIND) kind = 0;
        bf = BT_AUDIO_KIND_BURN_FIRST[kind]; bc = BT_AUDIO_KIND_BURN_COUNT[kind];
        hf = BT_AUDIO_KIND_HURT_FIRST[kind]; hc = BT_AUDIO_KIND_HURT_COUNT[kind];
    }
    float burn = bt_combat_audio_clip(s, bf, bc);      /* :672 the custom_sound pick */
    (void)bt_combat_audio_clip(s, hf, hc);             /* :298 the hurt pick, overridden */
    bt_combat_audio_positional(s, burn, BT_AUDIO_PITCH_RANGE_02, 0);
}

/* unit.gd:749-751 _on_one_shotted(effect) (the OneShotOnHitEffect roll succeeds
 * inside take_damage's hitbox-effect loop, BEFORE the hurt pick above):
 *
 *     SoundManager2D.play(effect.sound_one_shot, global_position, 0, 0.2)
 *
 * `effect.sound_one_shot` is a literal exported Resource -> NO selection draw,
 * so this helper only submits; normal positional, pitch_rand 0.2.  The only
 * shipped OneShotOnHitEffect rows are vorpal_sword tiers 2/3/4 (rows 837-839),
 * all carrying the same clip, so no row index is needed.
 *
 * Call it from the bt_wfx_one_shot() success path (melee + projectile hit
 * handling), immediately after the roll and before the primitive call. */
BT_HD static void bt_combat_audio_one_shot(BT_DEVICE BtState* s) {
    unsigned short first = BT_AUDIO_EV_FIRST[BT_AUDIO_EV_ONE_SHOT];
    unsigned short count = BT_AUDIO_EV_COUNT[BT_AUDIO_EV_ONE_SHOT];
    if (count == 0u) return;
    bt_combat_audio_positional(s, BT_AUDIO_CLIP_SECONDS[first], BT_AUDIO_PITCH_RANGE_02, 0);
}

/* ===========================================================================
 * Player (player.gd) + run-level main.gd events.
 * ========================================================================= */

/* player.gd:436-507 take_damage:
 *
 *     if dodgeable: disable_hurtbox(); _invincibility_timer.start(...)
 *     var sound = Utils.get_rand_element(hurt_sounds)       # draw 1 always
 *     if is_dodge:
 *         sound = Utils.get_rand_element(dodge_sounds)      # draw 2 on a dodge
 *         ... dodge effects (riposte / heal_on_dodge / temp stats) ...
 *     SoundManager2D.play(sound, global_position, 0, 0.2, true)   # FORCE
 *
 * Two draws on a dodge, one otherwise; the queued clip is the dodge clip when
 * dodged.  always_play = true -> bt_positional_audio_enqueue_force.
 *
 * Call it from bt_player_damage: on the dodge branch AFTER the dodge roll and
 * BEFORE bt_scalar_on_dodge / bt_temporal_on_dodge (the dodge-effect draws come
 * after the picks), and on the normal branch after `s->hp -= taken` (the source
 * picks after the health reduction and after the iframe start). */
BT_HD static void bt_combat_audio_player_hurt(BT_DEVICE BtState* s, int is_dodge) {
    float dur = bt_combat_audio_clip(s, BT_AUDIO_EV_FIRST[BT_AUDIO_EV_PLAYER_HURT],
                                     BT_AUDIO_EV_COUNT[BT_AUDIO_EV_PLAYER_HURT]);
    if (is_dodge) {
        /* `sound = Utils.get_rand_element(dodge_sounds)` REPLACES the hurt clip
         * unconditionally, exactly like the crit pick below. */
        unsigned short count = BT_AUDIO_EV_COUNT[BT_AUDIO_EV_PLAYER_DODGE];
        unsigned int idx = (count != 0u) ? bt_rng_bounded(&s->rng, (unsigned int)count) : 0u;
        dur = (count != 0u)
                  ? BT_AUDIO_CLIP_SECONDS[(unsigned int)BT_AUDIO_EV_FIRST[BT_AUDIO_EV_PLAYER_DODGE] + idx]
                  : 0.0f;
    }
    bt_combat_audio_positional(s, dur, BT_AUDIO_PITCH_RANGE_02, 1);
}

/* player.gd:730-745 on_healing_effect -- the single heal funnel (regen,
 * lifesteal, heal_on_kill, heal_when_pickup_gold, consumables, heal-over-time):
 *
 *     if value_healed > 0:
 *         SoundManager.play(Utils.get_rand_element(hp_regen_sounds), get_heal_db(), 0.1)
 *
 * ONE pick over hp_regen_sounds then a NORMAL main submission, pitch_rand 0.1
 * (`get_heal_db()` is a volume, never a draw).  Call it at the end of
 * bt_scalar_heal exactly when it returns > 0 -- that is the source's
 * `value_healed > 0` test (`heal()` returns 0 for a dead player / no_heal /
 * non-torture torture, and on_healing_effect returns 0 for negative or 0
 * healing), so the draw happens for every heal that actually restores HP. */
BT_HD static void bt_combat_audio_player_heal(BT_DEVICE BtState* s) {
    float dur = bt_combat_audio_clip(s, BT_AUDIO_EV_FIRST[BT_AUDIO_EV_HP_REGEN],
                                     BT_AUDIO_EV_COUNT[BT_AUDIO_EV_HP_REGEN]);
    bt_combat_audio_main(s, dur, BT_AUDIO_PITCH_RANGE_01, 0);
}

/* player.gd:850-875 on_alien_eyes_timeout:
 *
 *     var alien_stats = WeaponService.init_ranged_stats(...)   # no RNG draw
 *     SoundManager.play(Utils.get_rand_element(alien_sounds), 0, 0.1)
 *     for i in alien_eyes_effect[0]: ... manage_special_spawn_projectile(...)
 *
 * ONE pick per timer timeout (not per projectile), normal main submission,
 * pitch_rand 0.1.  Call it at the top of bt_scalar_alien_eyes_fire (the core
 * runs that helper exactly once per timeout, and only while the item is owned --
 * the source only creates the timer when alien_eyes is owned). */
BT_HD static void bt_combat_audio_alien_eyes(BT_DEVICE BtState* s) {
    float dur = bt_combat_audio_clip(s, BT_AUDIO_EV_FIRST[BT_AUDIO_EV_ALIEN_EYES],
                                     BT_AUDIO_EV_COUNT[BT_AUDIO_EV_ALIEN_EYES]);
    bt_combat_audio_main(s, dur, BT_AUDIO_PITCH_RANGE_01, 0);
}

/* main.gd:798-804 on_gold_picked_up (player_index >= 0):
 *
 *     SoundManager.play(Utils.get_rand_element(gold_pickup_sounds), 0, 0.2)
 *
 * ONE pick then a NORMAL main submission, pitch_rand 0.2.  ProgressData.settings
 * .alt_gold_sounds defaults to false (progress_data.gd:387,422), so the source's
 * `else` branch is the one that runs: BT_AUDIO_EV_GOLD_PICKUP.  The generated
 * BT_AUDIO_EV_GOLD_ALT_PICKUP span is the opt-in alternative and is deliberately
 * not consumed (the setting is a user preference, not simulated gameplay).
 * The pick happens BEFORE
 * apply_common_gold_pickup_effects / heal_when_pickup_gold / dmg_when_pickup_gold,
 * hence the call goes at the TOP of bt_scalar_gold_pickup (which is called once
 * per collected gold, after the already_picked_up guard). */
BT_HD static void bt_combat_audio_gold_pickup(BT_DEVICE BtState* s) {
    float dur = bt_combat_audio_clip(s, BT_AUDIO_EV_FIRST[BT_AUDIO_EV_GOLD_PICKUP],
                                     BT_AUDIO_EV_COUNT[BT_AUDIO_EV_GOLD_PICKUP]);
    bt_combat_audio_main(s, dur, BT_AUDIO_PITCH_RANGE_02, 0);
}

/* main.gd:863 on_levelled_up:
 *
 *     SoundManager.play(level_up_sound, 0, 0, true)
 *
 * `level_up_sound` is a literal exported Resource -> NO selection draw.  NORMAL
 * but always_play submission, pitch_rand 0.0.  The source fires it once per
 * level gained; call it inside bt_progress_award_levels' while loop. */
BT_HD static void bt_combat_audio_level_up(BT_DEVICE BtState* s) {
    unsigned short first = BT_AUDIO_EV_FIRST[BT_AUDIO_EV_LEVEL_UP];
    unsigned short count = BT_AUDIO_EV_COUNT[BT_AUDIO_EV_LEVEL_UP];
    if (count == 0u) return;
    bt_combat_audio_main(s, BT_AUDIO_CLIP_SECONDS[first], BT_COMBAT_PITCH_00, 1);
}

/* main.gd:958 clean_up_room() (runs at EVERY wave clear, and again from
 * _on_player_died when the last player dies):
 *
 *     SoundManager.play(Utils.get_rand_element(end_wave_sounds))
 *
 * ONE pick then a NORMAL main submission with the DEFAULT pitch_rand (0.0) and
 * always_play false.  Place it at the bottom of bt_progress_end_wave, i.e. after
 * manage_harvesting's draws and before TempStats.reset(), which is where the
 * source's clean_up_room() sits. */
BT_HD static void bt_combat_audio_end_wave(BT_DEVICE BtState* s) {
    float dur = bt_combat_audio_clip(s, BT_AUDIO_EV_FIRST[BT_AUDIO_EV_END_WAVE],
                                     BT_AUDIO_EV_COUNT[BT_AUDIO_EV_END_WAVE]);
    bt_combat_audio_main(s, dur, BT_COMBAT_PITCH_00, 0);
}

/* main.gd:1030 (the run was won):
 *
 *     SoundManager.play(Utils.get_rand_element(run_won_sounds), - 5, 0, true)
 *
 * ONE pick then an always_play main submission, pitch_rand 0.0.  The source
 * defers it behind the run-won screen animation; the core's wave-20 win branch
 * (brotato_core.h, `s->wave == BT_WAVES`) should call bt_combat_audio_end_wave
 * first (clean_up_room precedes it) and then this. */
BT_HD static void bt_combat_audio_run_won(BT_DEVICE BtState* s) {
    float dur = bt_combat_audio_clip(s, BT_AUDIO_EV_FIRST[BT_AUDIO_EV_RUN_WON],
                                     BT_AUDIO_EV_COUNT[BT_AUDIO_EV_RUN_WON]);
    bt_combat_audio_main(s, dur, BT_COMBAT_PITCH_00, 1);
}

/* main.gd:455 _on_player_died:
 *
 *     SoundManager.play(Utils.get_rand_element(run_lost_sounds), - 5, 0, true)
 *     ...
 *     if not live_players.empty(): return
 *     clean_up_room()          # -> the end-wave pick above (main.gd:958)
 *
 * ONE pick then an always_play main submission, pitch_rand 0.0.  The source
 * plays it on EVERY player death (before the live-player early return), and the
 * last death then reaches clean_up_room's end-wave pick on the same call stack.
 * For a single-player run that is run-lost pick, then end-wave pick. */
BT_HD static void bt_combat_audio_run_lost(BT_DEVICE BtState* s) {
    float dur = bt_combat_audio_clip(s, BT_AUDIO_EV_FIRST[BT_AUDIO_EV_RUN_LOST],
                                     BT_AUDIO_EV_COUNT[BT_AUDIO_EV_RUN_LOST]);
    bt_combat_audio_main(s, dur, BT_COMBAT_PITCH_00, 1);
}

/* player.gd:395-408 on_weapon_broken (a weapon's break_on_hit fired):
 *
 *     ... emit wanted_to_spawn_gold; RunData.remove_weapon_by_index(...) ...
 *     SoundManager.play(Utils.get_rand_element(WeaponService.breaking_sounds),
 *                       - 15, 0.1, true)
 *
 * ONE pick then an always_play main submission, pitch_rand 0.1.  The source does
 * nothing (no draw, no sound) when the weapon is no longer in current_weapons,
 * so a break credited with no live owning slot submits nothing.  Call it at the
 * break sites (after the break gold spawn, which draws its own loot arc), not
 * inside bt_weapon_remove -- that same function also serves shop recycling and
 * combining, which never play it. */
BT_HD static void bt_combat_audio_weapon_break(BT_DEVICE BtState* s) {
    float dur = bt_combat_audio_clip(s, BT_AUDIO_EV_FIRST[BT_AUDIO_EV_WEAPON_BREAK],
                                     BT_AUDIO_EV_COUNT[BT_AUDIO_EV_WEAPON_BREAK]);
    bt_combat_audio_main(s, dur, BT_AUDIO_PITCH_RANGE_01, 1);
}

/* player.gd:544-560 explode(stats, effect, nb_explosions) -- every
 * explode_on_hit / explode_when_below_hp effect:
 *
 *     if not Utils.get_chance_success(effect.chance): return   # already rolled
 *     ...
 *     if stats.shooting_sounds.size() > 0:
 *         SoundManager2D.play(Utils.get_rand_element(stats.shooting_sounds),
 *                             global_position, stats.sound_db_mod, 0.2, true)
 *
 * THE CHARACTER EXPLOSION ONLY.  player.gd:560 reads the shooting_sounds of the
 * EXPLODING EFFECT's own stats row.  The Bull's character explosion is modelled
 * by bt_explode, which owns no stats row, and bull_explosion_stats.tres has an
 * empty shooting_sounds array, so this zero-count event is exactly right for it.
 * Every ITEM-carried explosion goes through the row-addressed sibling
 * bt_combat_audio_player_explosion_row below instead, so no row is dropped.
 *
 * Call it from bt_explode right after the character/chance guard, before the
 * crit roll. */
BT_HD static void bt_combat_audio_player_explosion(BT_DEVICE BtState* s) {
    unsigned short first = BT_AUDIO_EV_FIRST[BT_AUDIO_EV_BULL_EXPLODE];
    unsigned short count = BT_AUDIO_EV_COUNT[BT_AUDIO_EV_BULL_EXPLODE];
    if (count == 0u) return;   /* size()>0 */
    unsigned int idx = bt_rng_bounded(&s->rng, (unsigned int)count);
    bt_combat_audio_positional(s, BT_AUDIO_CLIP_SECONDS[(unsigned int)first + idx],
                               BT_AUDIO_PITCH_RANGE_02, 1);
}

/* player.gd:559-560 for an ITEM-carried explosion -- `stats_row` is the
 * BT_ITEM_WSTATS row of the ExplodingEffect's own `stats` resource (an
 * ItemExplodingEffect's per-copy cursed stats; e.g. krakens_eye ->
 * krakens_eye_explosion_stats, whose shooting_sounds is EMPTY, and sunken_bell ->
 * sunken_bell_explosion_stats, whose 1 clip IS the bell ring).  Same guarded
 * pick + always_play positional submission as above, but the span comes from the
 * canonical BT_AUDIO_WSTATS_SHOOT_* table so each effect's own stats row decides
 * both the draw and the clip.  count 0 => the source's size()>0 guard is false:
 * no draw, no submission.
 *
 * Call it from the per-copy player explosion spawn (brotato_item_explosions.h
 * bt_item_expl_player_copy) with the row's E->stats. */
BT_HD static void bt_combat_audio_player_explosion_row(BT_DEVICE BtState* s, int stats_row) {
    if (stats_row < 0 || stats_row >= BT_NAUDIO_WSTATS_SHOOT) return;
    unsigned short count = BT_AUDIO_WSTATS_SHOOT_COUNT[stats_row];
    if (count == 0u) return;   /* size()>0 */
    unsigned int idx = bt_rng_bounded(&s->rng, (unsigned int)count);
    bt_combat_audio_positional(s,
        BT_AUDIO_CLIP_SECONDS[(unsigned int)BT_AUDIO_WSTATS_SHOOT_FIRST[stats_row] + idx],
        BT_AUDIO_PITCH_RANGE_02, 1);
}

/* player_explosion.gd:28-32 (the PlayerExplosion NODE, whose root scenes are
 * explosion.tscn and flesh_explosion.tscn):
 *
 *     if not sound_played:
 *         sound_played = true
 *         set_physics_process(false)
 *         if sound_db_mod > - 50:
 *             SoundManager2D.play(Utils.get_rand_element(explosion_sounds),
 *                                 global_position, sound_db_mod, 0.2)
 *
 * `scene_row` is the BT_ITEM_SCENES row of the node's scene (the generated
 * BT_AUDIO_EXPLNODE_* tables are keyed by that row).  ONE pick then a NORMAL
 * positional submission at pitch_rand 0.2; count 0 = the scene defines no
 * explosion_sounds, so nothing is drawn and nothing is submitted.
 *
 * The caller owns WHEN: a node sounds on its first physics frame and, because
 * `end_explosion` resets sound_played WITHOUT re-enabling _physics_process, a
 * POOLED node never sounds again -- so bt_item_expl_tick emits this once per
 * FRESH node instantiation, before that frame's hit resolutions. */
BT_HD static void bt_combat_audio_explosion_node(BT_DEVICE BtState* s, int scene_row) {
    if (scene_row < 0 || scene_row >= BT_NAUDIO_EXPLNODE_SCENES) return;
    unsigned short count = BT_AUDIO_EXPLNODE_COUNT[scene_row];
    if (count == 0u) return;
    unsigned int idx = bt_rng_bounded(&s->rng, (unsigned int)count);
    bt_combat_audio_positional(s,
        BT_AUDIO_CLIP_SECONDS[(unsigned int)BT_AUDIO_EXPLNODE_FIRST[scene_row] + idx],
        BT_AUDIO_PITCH_RANGE_02, 0);
}

/* singletons/weapon_service.gd:339-352 manage_special_spawn_projectile -- the
 * weapon/service projectile carriers that are NOT the ordinary weapon shot
 * (`projectiles_on_hit` children, e.g. lightning_shiv / dextroyer;
 * `projectiles_on_death` children, e.g. baby_with_a_beard; the alien-eyes shot):
 *
 *     func manage_special_spawn_projectile(entity_from, weapon_stats, direction,
 *                                          auto_target_enemy, ...):
 *         var pos = entity_from.global_position
 *         if weapon_stats.shooting_sounds.size() > 0:
 *             SoundManager2D.play(Utils.get_rand_element(weapon_stats.shooting_sounds),
 *                                 pos, 0, 0.2)
 *         if auto_target_enemy:
 *             var target = entity_spawner_ref.get_rand_enemy(entity_from)   # draw
 *
 * `stats_row` is the BT_ITEM_WSTATS row of `weapon_stats` (unit.gd:589 passes
 * `hitbox.projectiles_on_hit[1]`, main.gd:523 `projectiles_on_death[1]`,
 * player.gd:875 `alien_eyes_stats`).  count 0 (cactus_mace_projectile,
 * base_ranged_weapon_stats, alien_eyes_stats) skips both the pick and the play;
 * otherwise ONE pick and a NORMAL positional submission, pitch_rand 0.2.
 *
 * DRAW ORDER at every call site (the parent's `ALWAYS direction, THEN sound,
 * THEN auto_target`): the CALLER evaluates `rand_range(-PI, PI)` for the
 * direction first, then this helper's pick, then `get_rand_enemy`.  So call it
 * per CHILD, between the direction draw and the auto-target draw -- the source
 * calls manage_special_spawn_projectile once per element of
 * `projectiles_on_hit[0]` / `projectiles_on_death[0]`, not once per hitbox.
 *
 * This is a positional (SoundManager2D) event and is NOT the ranged/melee
 * shooting_behavior shot that brotato_weapon_audio.h already owns. */
BT_HD static void bt_combat_audio_special_projectile(BT_DEVICE BtState* s, int stats_row) {
    if (stats_row < 0 || stats_row >= BT_NAUDIO_WSTATS_SHOOT) return;
    unsigned short count = BT_AUDIO_WSTATS_SHOOT_COUNT[stats_row];
    if (count == 0u) return;
    unsigned int idx = bt_rng_bounded(&s->rng, (unsigned int)count);
    float dur = BT_AUDIO_CLIP_SECONDS[(unsigned int)BT_AUDIO_WSTATS_SHOOT_FIRST[stats_row] + idx];
    bt_combat_audio_positional(s, dur, BT_AUDIO_PITCH_RANGE_02, 0);
}

