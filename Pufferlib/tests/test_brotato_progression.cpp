/* Consumer-visible progression transitions: XP queues upgrades during combat,
 * choices precede the shop, and the shop exits only through action 8. */
#include "../ocean/brotato/brotato_core.h"
#include <cstdio>
#include <cstring>
#include <cmath>
static BtState state;
static int failures;
static void check(bool valid, const char *message) {
    if (!valid) { std::fprintf(stderr, "FAIL: %s\n", message); failures++; }
}
static bool near(float a, float b) { return std::fabs(a-b) < 0.0001f; }
/* lvl_offers[] stores (BT_UPGRADES row + 1) so 0 is the unambiguous "no offer"
 * marker (the sim-side encoding change); decode it here. */
static int row_of(unsigned short off) { return (int)off - 1; }
static int item(const char *name) {
    for (int i=0; i<BT_NITEMS; i++) if (!std::strcmp(BT_ITEMS[i].name,name)) return i;
    std::fprintf(stderr,"Missing item: %s\n",name); failures++; return -1;
}
static void reset() { bt_core_init(&state, 1234, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE); state.n_live_groups=0; }
/* main.tscn: EndWaveTimer 2.0 s = 120 ticks, HarvestingTimer 0.66 s = 40 ticks,
 * WaveClearedLabel/Timer 0.1 s = 6 ticks x 14 characters.  The shop/level-up
 * transition (RunData.on_wave_end) fires only at the EndWaveTimer timeout, so
 * `bt_progress_end_wave` alone leaves phase 0 with the window armed.
 *
 * A window FRAME is bt_progress_wave_end_step PLUS the killed nodes' death
 * animation: bt_core_step runs that in its enemy block (brotato_core.h, the
 * bt_enemy_dying_step call below the live-enemy loop), which is exactly one tick
 * per frame -- the window step does not tick it again. */
static void window_frame() {
    bt_enemy_dying_step(&state);
    bt_progress_wave_end_step(&state);
}
static void advance_window(int ticks) {
    for (int i = 0; i < ticks; i++) window_frame();
}
static void end_wave_full() {
    bt_progress_end_wave(&state);
    advance_window(120);
}
int main() {
    reset();
    bt_progress_add_xp(&state,15);
    check(state.level==0 && state.pending_upgrades==0 && near(state.xp,15),"XP below threshold does not grant a level");
    bt_progress_add_xp(&state,1);
    check(state.level==1 && state.pending_upgrades==1 && state.phase==0 && near(state.xp,0),"level and queued upgrade are immediate; combat remains active");
    check(near(state.max_hp,31) && state.xp_need==25,"level grants HP and advances next XP threshold");
    bt_progress_add_xp(&state,61);
    check(state.level==3 && state.pending_upgrades==3 && near(state.xp,0),"one pickup can earn multiple queued upgrades");
    bt_progress_end_wave(&state);
    check(state.phase==0 && state.end_wave_active==1 && state.end_wave_timer==BT_END_WAVE_TICKS,
          "the expiry frame keeps phase 0 and arms the 2 s EndWaveTimer");
    advance_window(119);
    check(state.phase==0, "phase stays 0 for the whole 2 s window (119 ticks in)");
    advance_window(1);
    check(state.phase==2 && state.pending_upgrades==3,
          "the flip lands on tick 120 and opens queued upgrade choices before shop");
    int offered_level=state.level;
    for (int i=0;i<3;i++) bt_progress_choose_upgrade(&state,0);
    check(state.phase==1 && state.pending_upgrades==0 && state.level==offered_level,"choosing upgrades neither consumes XP nor awards levels twice");

    reset(); state.scalar_effect[BT_ISTR_XP_GAIN]=100;
    bt_progress_add_xp(&state,8);
    check(state.level==1 && near(state.xp,0),"XP gain modifier applies before threshold processing");

    reset();
    bt_core_stat_add(&state,12,20);
    float starting_materials=state.materials;
    bt_progress_end_wave(&state);
    check(near(state.materials,starting_materials+20) && state.level==1 && near(state.xp,4),"harvesting awards both material and XP before upgrade choices");
    check(near(state.harvest,20) && state.phase==0,"the growth runs on the HarvestingTimer, not in the expiry frame");
    advance_window(39);
    check(near(state.harvest,20) && state.phase==0,"no growth before tick 40");
    advance_window(1);
    check(near(state.harvest,21),"the HarvestingTimer fires on tick 40: 20 + ceil(20*5%)");
    /* the label draws its 14 characters at ticks 6,12,..,84; the window is over
     * at 120, so 41 further ticks are drawing-free but still phase 0. */
    advance_window(80 - 40);
    check(state.phase==0,"the label run finishes well inside the window");
    advance_window(40);
    check(state.phase==2,"positive harvesting queues its earned upgrade at the window end");

    reset(); state.phase=1; state.shop_timer=0; state.shop_steps=1;
    int coupon=item("coupon");
    if (coupon>=0) {
        state.shop_kind[0]=state.shop_kind[1]=BT_SHOP_ITEM;
        state.shop_id[0]=state.shop_id[1]=(unsigned short)coupon;
        state.shop_available=3; state.shop_cursed[0]=state.shop_cursed[1]=0;
        state.materials=1000;
        float first=bt_shop_price(&state,(unsigned short)coupon);
        bt_progress_shop_action(&state,0);
        check(state.phase==1 && state.shop_available==2 && near(state.materials,1000-first),"buying consumes one slot and currency without leaving shop");
        float remaining=state.materials;
        bt_progress_shop_action(&state,0);
        check(near(state.materials,remaining),"a consumed slot cannot be bought twice");
        float discounted=bt_shop_price(&state,(unsigned short)coupon);
        float raw=(float)BT_ITEMS[coupon].price+1+(float)BT_ITEMS[coupon].price*0.1f;
        check(near(discounted,(float)(int)(raw*0.95f)),"coupon changes subsequent live prices");
        state.materials=0;
        bt_progress_shop_action(&state,1);
        check(state.shop_available==2 && near(state.materials,0),"unaffordable purchase preserves the offer");
        state.materials=1000;
        bt_progress_shop_action(&state,1);
        check(state.phase==1 && state.shop_available==0,"buying final offer still requires explicit exit");
    }

    reset(); state.phase=1; state.wave=8; state.shop_timer=0;
    state.materials=100; state.free_rerolls_remaining=1;
    bt_progress_shop_action(&state,4);
    check(near(state.materials,100) && state.paid_rerolls==0 && state.free_rerolls_remaining==0,"free reroll does not advance paid price");
    bt_progress_shop_action(&state,4);
    check(near(state.materials,91) && state.paid_rerolls==1,"first paid reroll charges base cost after free reroll");
    bt_progress_shop_action(&state,4);
    check(near(state.materials,79) && state.paid_rerolls==2,"subsequent paid reroll advances source price");

    reset(); state.phase=1; state.shop_timer=0;
    bt_core_stat_add(&state,0,10); bt_core_stat_add(&state,2,7);
    state.hp=2; state.px=500; state.py=600; state.iframe=1;
    bt_progress_shop_action(&state,8);
    bt_progress_shop_go(&state);   /* change_scene is deferred to the GO frame's end */
    check(state.phase==0 && state.wave==2,"skip is the explicit next-wave transition");
    check(near(state.max_hp,40) && near(state.armor,17) && near(state.hp,40),"new wave restores health without resetting purchased permanent stats");
    check(near(state.px,BT_ARENA_W*0.5f) && near(state.py,BT_ARENA_H*0.5f) && near(state.iframe,0),"new arena resets player position and invulnerability");

    reset(); state.phase=1; state.shop_timer=0;
    state.scalar_effect[BT_ISTR_HP_START_WAVE]=-50;
    bt_progress_shop_action(&state,8);
    bt_progress_shop_go(&state);
    check(near(state.hp,15),"persistent wave-start HP modifier applies");
    state.phase=1; state.shop_timer=0;
    state.scalar_effect[BT_ISTR_HP_START_NEXT_WAVE]=-25;
    bt_progress_shop_action(&state,8);
    bt_progress_shop_go(&state);
    check(near(state.hp,22) && near(state.scalar_effect[BT_ISTR_HP_START_NEXT_WAVE],0),"one-wave HP modifier overrides persistent modifier and is consumed");
    /* _old_upgrades across the level-ups of ONE end-of-wave session
     * (upgrades_ui_player_container.gd:37,65-66 + item_service.gd:561-565):
     * the offers of the previous level-up are excluded from the next one's, by
     * upgrade_id (== the stat, so a different TIER of a stat still collides). */
    reset();
    state.level = 5; state.pending_upgrades = 2;
    end_wave_full();
    check(state.phase == 2 && state.pending_upgrades == 2, "two queued levels open the level-up phase");
    {
        int first[4], second[4];
        for (int i = 0; i < 4; i++) first[i] = state.lvl_offers[i];
        for (int i = 0; i < 4; i++)
            for (int j = i + 1; j < 4; j++)
                check(BT_UPGRADES[row_of(first[i])].stat != BT_UPGRADES[row_of(first[j])].stat,
                      "the four offers of one level-up carry distinct stats");
        bt_progress_choose_upgrade(&state, 0);
        check(state.pending_upgrades == 1, "the choice leaves the second level queued");
        for (int i = 0; i < 4; i++) second[i] = state.lvl_offers[i];
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                check(BT_UPGRADES[row_of(first[i])].stat != BT_UPGRADES[row_of(second[j])].stat,
                      "no stat offered at the previous level-up is offered again");
        /* level 5 forces Tier.UNCOMMON while level 4 draws COMMON, so the two
         * sets come from different tiers -- the collision is on the stat. */
        check(BT_UPGRADES[row_of(first[0])].tier == 1, "level 4 draws the COMMON tier");
        check(BT_UPGRADES[row_of(second[0])].tier == 2, "level 5 forces the UNCOMMON tier");
    }

    /* The empty _old_upgrades set must exclude NOTHING: an absent slot (0) is
     * not BT_UPGRADES row 0 (stat_max_hp).  Without the row+1 encoding,
     * stat_max_hp could never be offered at the first level-up of a wave. */
    {
        int saw_max_hp = 0;
        for (bt_u64 seed = 1; seed <= 40 && !saw_max_hp; seed++) {
            bt_core_init(&state, seed, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE);
            state.n_live_groups = 0;
            state.level = 1; state.pending_upgrades = 1;
            end_wave_full();
            for (int i = 0; i < 4; i++)
                if (BT_UPGRADES[row_of(state.lvl_offers[i])].stat == 0) saw_max_hp = 1;
        }
        check(saw_max_hp, "stat_max_hp is offerable with an empty _old_upgrades set");
    }

    /* manage_harvesting (main.gd:1246-1284): the total is granted as gold AND
     * XP, truncated to int by add_gold/add_xp, and the growth uses the float
     * stat.  Past the last wave the growth becomes a 20% loss. */
    reset();
    bt_core_stat_add(&state, 12, 7.9f);          /* stat_harvesting 7.9 */
    state.wave = 10;
    {
        float m = state.materials;
        bt_progress_end_wave(&state);
        check(near(state.materials, m + 7.0f), "add_gold truncates the harvesting total to int");
        check(near(state.xp, 7.0f), "the same total is granted as XP (gold and XP are 1:1)");
        advance_window(40);
        check(near(state.harvest, 8.9f), "growth uses the untruncated stat: 7.9 + ceil(7.9 * 5%)");
    }
    reset();
    bt_core_stat_add(&state, 12, 100.0f);
    state.wave = 20;
    bt_progress_end_wave(&state); advance_window(40);
    check(near(state.harvest, 105.0f), "through wave 20 harvesting grows by ceil(stat * 5%)");
    state.wave = 21;
    bt_progress_end_wave(&state); advance_window(40);
    check(near(state.harvest, 84.0f),
          "past the last wave harvesting SHRINKS by ceil(stat * ENDLESS_HARVESTING_DECREASE%)");

    /* main.gd:931 clean_up_room -> init_elites_spawn(current_wave+10, 0.0): the
     * wave-10 (and every wave % 10 == 0 >= 10) reschedule APPENDS a second elite
     * schedule; no phase transition is needed to arm it. */
    reset();
    state.wave = 10; state.end_wave_active = 0;
    {
        int before = (int)state.oe_n;
        bt_progress_end_wave(&state);
        check((int)state.oe_n > before,
              "the wave-10 expiry appends the next decade's elite schedule");
    }

    /* Level-up reroll (upgrades_ui_player_container.gd:57-75,180-199): the
     * container's own counter prices the reroll off get_reroll_price(wave,count).
     * Its initial price is current_wave * 0.75 + delta; a paid reroll increments
     * the count first and charges the NEXT price. */
    reset();
    state.phase = 2; state.wave = 8; state.level = 3; state.pending_upgrades = 1;
    state.materials = 100000;
    bt_progress_roll_upgrade(&state);
    state.lvl_reroll_count = 0;
    {
        int p0 = bt_progress_lvl_reroll_price(&state);
        check(p0 == bt_progress_lvl_reroll_price(&state), "the level-up reroll price is deterministic");
        float m = state.materials;
        bt_progress_choose_upgrade(&state, BT_ACT_REROLL);
        check(near(state.materials, m - (float)p0) && state.lvl_reroll_count == 1,
              "a paid level-up reroll charges the current price and advances the counter");
        int p1 = bt_progress_lvl_reroll_price(&state);
        check(p1 >= p0, "the price is non-decreasing with the counter");
    }

    /* item_service.gd:550-556 single-offer early return: owning
     * weapon_slot_upgrades above the current weapon_slot forces ONE offer, and
     * choosing it raises the weapon_slot scalar. */
    reset();
    state.level = 3; state.pending_upgrades = 1;
    state.scalar_effect[BT_ISTR_WEAPON_SLOT] = 2;
    state.scalar_effect[BT_ISTR_WEAPON_SLOT_UPGRADES] = 3;
    bt_progress_roll_upgrade(&state);
    check(state.lvl_offers[0] == (unsigned short)BT_UPGRADE_SLOT_OFFER &&
          state.lvl_offers[1] == 0,
          "weapon_slot_upgrades returns exactly one offer (slots 1..3 absent, 0)");
    {
        float before = state.scalar_effect[BT_ISTR_WEAPON_SLOT];
        bt_progress_choose_upgrade(&state, 0);
        check(near(state.scalar_effect[BT_ISTR_WEAPON_SLOT], before + BT_UPGRADE_SLOT_VALUE),
              "choosing it adds one weapon slot");
    }
    /* the reroll action is not shown for a single offer (button hidden) */
    reset();
    state.level = 3; state.pending_upgrades = 1; state.materials = 100000;
    state.scalar_effect[BT_ISTR_WEAPON_SLOT] = 2;
    state.scalar_effect[BT_ISTR_WEAPON_SLOT_UPGRADES] = 3;
    bt_progress_roll_upgrade(&state);
    {
        float m = state.materials;
        bt_progress_choose_upgrade(&state, BT_ACT_REROLL);
        check(near(state.materials, m) && state.pending_upgrades == 1,
              "the hidden reroll button is a no-op while the single offer is shown");
        bt_progress_choose_upgrade(&state, 2);
        check(state.pending_upgrades == 1, "slots with no choice node are no-ops");
    }

    /* item_service.gd:578-586 + bt_core_observe[27..34]: level_upgrades_modifications
     * scales the OFFER at generation, so the observed magnitude is the modified
     * one (e.g. max_hp tier-1 value 3 -> 4 at +35%). */
    reset();
    state.phase = 2; state.level = 3; state.pending_upgrades = 1;
    state.scalar_effect[BT_ISTR_LEVEL_UPGRADES_MODIFICATIONS] = 35;
    bt_progress_roll_upgrade(&state);
    {
        float obs[BT_OBS_SIZE];
        bt_core_observe(&state, obs);
        int slot0_stat = (int)BT_UPGRADES[row_of(state.lvl_offers[0])].stat;
        float want = bt_clampf((float)(int)(BT_UPGRADES[row_of(state.lvl_offers[0])].value * 1.35f) * 0.1f, -1.0f, 1.0f);
        check(near(obs[27], (float)slot0_stat / 12.0f), "offer stat is unchanged by the modifier");
        check(near(obs[28], want), "the observed offer value carries level_upgrades_modifications");
    }

    /* The sim-side row+1 encoding closes the artifact's last mask gap: a present
     * slot stores (BT_UPGRADES row + 1) and 0 is unambiguously absent, so the
     * v5 presence flag (obs[199..202]) and the ban-token budget (obs[198]) carry
     * exactly the two conditions the v4 observation could not express. */
    reset();
    state.phase = 2; state.level = 3; state.pending_upgrades = 1;
    bt_progress_roll_upgrade(&state);
    {
        float obs[BT_OBS_SIZE];
        bt_core_observe(&state, obs);
        for (int i = 0; i < 4; i++) {
            check(state.lvl_offers[i] >= 1 && state.lvl_offers[i] <= BT_NUPGRADES,
                  "a rolled offer stores (BT_UPGRADES row + 1)");
            check(obs[199 + i] == 1.0f, "a present level-up slot observes presence 1.0");
        }
        state.lvl_offers[3] = 0;                    /* absent slot */
        bt_core_observe(&state, obs);
        check(obs[202] == 0.0f, "an absent level-up slot observes presence 0.0");
        check(near(obs[33], 0.0f) && near(obs[34], 0.3f),
              "an absent slot keeps the core's empty-slot encoding (BT_UPGRADES[0])");
        /* The row-0 offer (max_hp tier 1 stat 0 value 3) must not read as absent. */
        state.lvl_offers[3] = 1;
        bt_core_observe(&state, obs);
        check(obs[202] == 1.0f, "BT_UPGRADES row 0 (stored as 1) is a present offer");
    }
    {
        float obs[BT_OBS_SIZE];
        state.ban_mode = 1; state.ban_tokens = 4;
        bt_core_observe(&state, obs);
        check(near(obs[198], 0.5f), "obs[198] = ban_tokens / BT_BAN_MAX_TOKEN");
        state.ban_mode = 0;
        bt_core_observe(&state, obs);
        check(obs[198] == 0.0f, "ban mode off -> obs[198] is 0.0");
    }
    /* An absent slot has no choice node and the reroll button is hidden with a
     * single offer: both are no-ops, matching bt_action_legal. */
    reset();
    state.phase = 2; state.level = 3; state.pending_upgrades = 1; state.materials = 100000;
    bt_progress_roll_upgrade(&state);
    for (int i = 1; i < 4; i++) state.lvl_offers[i] = 0;
    state.lvl_offers[0] = 2;
    {
        int before = state.pending_upgrades;
        bt_progress_choose_upgrade(&state, 2);
        check(state.pending_upgrades == before, "choosing an absent level-up slot is a no-op");
        bt_progress_choose_upgrade(&state, BT_ACT_REROLL);
        check(state.pending_upgrades == before, "the reroll is a no-op with a single offer");
    }

    /* run_data.gd:767-768 get_next_level_xp_needed: (level+4)^2 * (1 + pct/100)
     * in binary64.  BT_CHARACTERS ships only Bull/Knight/Demon, whose
     * next_level_xp_pct is 0, so the multiplier is exercised through the shared
     * binary64 helper and the FRACTIONAL bound through an injected threshold. */
    {
        float want = 36.8f;   /* 16 * 2.3 */
        float got = bt_rf64_to_float(bt_rf64_mul(bt_rf64_from_float(16.0f),
                                                bt_rf64_one_plus_pct(130.0f)));
        check(near(got, want), "16 * (1 + 130/100) = 36.8 in binary64");
        check(near(bt_xp_needed_pct(BT_CHARACTER_DEMON, 0), 16.0f),
              "a 0-pct profile keeps the integer curve");
        reset();
        state.xp_need = want;
        bt_progress_add_xp(&state, 36);
        check(state.level == 0 && near(state.xp, 36.0f), "36 XP is below a 36.8 threshold");
        bt_progress_add_xp(&state, 1);
        check(state.level == 1, "37 XP crosses the fractional 36.8 threshold");
        check(near(state.xp_need, 25.0f), "the next threshold is the integer curve again");
    }

    /* main.gd:1232 clean_up_room -> entity_spawner.gd:580-608: the wave end kills
     * every live enemy with the spawner's cleaning_up die-args, so the LIVING
     * count snaps to 0 on the expiry frame while the NODES stay observable for the
     * death animation.  That is the game's own window (v4_demon_s1 wave 1:
     * `enemies` 5 -> 0 and `living_enemies` 5 -> 0 on the expiry tick, `enemy_nodes`
     * 5 / obs[67] 0.05 on rows 179..239 and 0 from row 240: 61 rows, one more than
     * a weapon kill's 60 because this die() runs from the engine's IDLE pass --
     * see BT_POOL_DEATH_IDLE_EXTRA). */
    reset();
    state.wave = 3;
    {
        int kills0 = state.kills;
        float obs[BT_OBS_SIZE];
        state.matcount = 0;
        for (int m = 0; m < BT_MAX_MAT; m++) state.matalive[m] = 0;
        for (int c = 0; c < BT_MAX_CONS; c++) state.conalive[c] = 0;
        bt_spawn_kind_at(&state, 0, 0, state.px + 40.0f, state.py, 0);
        bt_spawn_kind_at(&state, 1, 0, state.px - 40.0f, state.py, 0);
        state.e_hold_tick[0] = state.e_hold_tick[1] = 0xFFFFu;
        check(state.ecount == 2, "two live enemies before the expiry frame");
        bt_progress_end_wave(&state);
        check(state.ecount == 0, "the expiry frame kills them: the living count snaps to 0");
        check(state.kills == kills0, "cleaning_up credits no kill");
        check(state.matcount == 0, "cleaning_up spawns no loot");
        bt_core_observe(&state, obs);
        check(near(obs[67], 2.0f * BT_ENEMY_COUNT_SCALE),
              "the dying nodes are still observable (obs[67] counts nodes, not the living)");
        check(state.edying[0] == BT_POOL_DEATH_FRAMES + BT_POOL_DEATH_IDLE_EXTRA &&
              state.edying[1] == BT_POOL_DEATH_FRAMES + BT_POOL_DEATH_IDLE_EXTRA,
              "the killed nodes entered the idle-pass death animation (one row longer)");
        for (int t = 0; t < BT_POOL_DEATH_FRAMES + BT_POOL_DEATH_IDLE_EXTRA - 1; t++)
            window_frame();
        bt_core_observe(&state, obs);
        check(near(obs[67], 2.0f * BT_ENEMY_COUNT_SCALE),
              "the nodes stay observable through the whole window");
        window_frame();
        bt_core_observe(&state, obs);
        check(near(obs[67], 0.0f), "the nodes are freed when the death animation ends");
        check(state.ecount == 0, "the living count stays 0 through the window");
    }

    /* player.gd:807-822 _clean_up() (player.on_room_cleanup from the expiry
     * frame): cleaning_up = true, _can_move = false, _current_movement = ZERO and
     * set_physics_process(false).  So the expiry frame itself still integrates
     * that frame's action (the timer callback runs after the physics step), and
     * every following window frame ignores the injected movement; the regen and
     * invincibility timers are paused too, and movement resumes only in the next
     * wave's fresh Main (v4_demon_s1: obs[4]/obs[5] diverge from row 181 until
     * this lock was modelled). */
    reset();
    state.wave = 3;
    {
        float obs[BT_OBS_SIZE], rew = 0.0f, term = 0.0f;
        state.time_left = 0.5f * BT_DT;         /* the expiry frame is this step */
        state.iframe = 30.0f;
        float x0 = state.px, y0 = state.py;
        bt_core_step(&state, 7, obs, &rew, &term);
        check(state.end_wave_active == 1, "the expiry frame arms the window");
        check(state.px != x0 || state.py != y0,
              "the expiry frame still integrates that frame's action");
        float x1 = state.px, y1 = state.py;
        float iframe0 = state.iframe;
        for (int t = 0; t < 90; t++) bt_core_step(&state, 7, obs, &rew, &term);
        check(near(state.px, x1) && near(state.py, y1),
              "the window locks the player: the injected movement is ignored");
        check(near(state.iframe, iframe0),
              "the window pauses the invincibility timer (obs[6] frozen)");
        while (state.end_wave_active) bt_core_step(&state, 0, obs, &rew, &term);
        check(state.phase == 1, "the window ends in the shop");
        bt_core_step(&state, 8, obs, &rew, &term);            /* Go -> next wave */
        /* base_shop.gd:339-349 ends in change_scene(), applied at the frame's
         * end: the GO frame still observes the Shop with the wave advanced, and
         * the new arena exists from the following tick. */
        check(state.phase == 1 && state.wave == 4,
              "the GO frame still shows the shop at the advanced wave");
        bt_core_step(&state, 0, obs, &rew, &term);
        check(state.phase == 0 && state.wave == 4, "Go starts the next wave");
        float rx = state.px, ry = state.py;
        bt_core_step(&state, 7, obs, &rew, &term);
        check(state.px != rx || state.py != ry, "movement resumes in the new wave");
    }

    /* The GO frame builds the next arena but does not run it.  change_scene_to
     * defers `_change_scene` with call_deferred (scene/main/scene_tree.cpp:
     * 1550-1558) and SceneTree::iteration flushes the deferred queue AFTER its
     * physics_process walk (:605-607), so the GO frame's own physics belongs to
     * the scene being torn down (the Shop: no player, no arena) and every node of
     * the fresh Main -- Player, its Weapon nodes, the spawner -- runs its first
     * _physics_process in the NEXT frame.  The new WaveTimer does carry into the
     * creation frame, because Godot's Timer defaults to TIMER_PROCESS_IDLE and
     * the scene entered the tree before that frame's idle walk: its clock has
     * already ticked once when the next frame observes it.  Measured on the real
     * engine (v5_demon_s1 wave 2): row 309 = the creation row reads obs[82] = 1.0
     * -- the untouched wave-begin cooldown draw -- with wave_time_left =
     * duration - 1/60, and row 310 is the first row showing one decrement
     * (12/13).  So the creation row must consume exactly one wave-clock frame and
     * NO arena frame, like a fresh bt_core_reset for a run's wave 1. */
    printf("[wave-start] the GO frame builds the next arena without running it\n");
    {
        const int WEAPON_BASE = BT_OBS_PLAYER + BT_OBS_ITEMS + BT_OBS_UPGRADES +
                                BT_OBS_ENEMIES + BT_OBS_WAVE + BT_OBS_PROJ + BT_OBS_CONS;
        static const char* weapons[2] = { "weapon_ghost_scepter_1", "weapon_knife_1" };
        for (int w = 0; w < 2; w++) {
            int wid = bt_weapon_index_from_id(weapons[w]);
            check(wid > 0, "canonical starting weapon resolves");
            bt_core_init(&state, 1234, 0, 1, BT_CHARACTER_DEMON, (unsigned short)wid);
            state.n_live_groups = 0;
            check(state.wpn_count == 1 && state.wpn_id[0] == (unsigned short)wid,
                  "the profile's starting weapon is in slot 0");
            float obs[BT_OBS_SIZE], rew = 0.0f, term = 0.0f;
            state.phase = 1; state.shop_timer = 0;   /* wave 1's shop is this frame */
            bt_core_step(&state, 8, obs, &rew, &term);            /* Go */
            check(state.phase == 1 && state.wave == 2,
                  "the GO frame still observes the shop at the advanced wave");
            int ticks_before = state.ticks;
            bt_core_step(&state, 0, obs, &rew, &term);   /* the GO frame's tail */
            check(state.phase == 0 && state.wave == 2,
                  "the creation frame builds the new wave's arena");
            check(state.tick_in_wave == 0 && state.ticks == ticks_before,
                  "the creation frame runs no arena frame (no in-wave tick, no combat tick)");
            check(near(state.time_left, state.wave_duration - BT_DT),
                  "the creation frame consumes exactly one wave-clock frame");
            check(state.cur_spawn_delay == 0, "the spawner takes no creation-frame step");
            BtPlayerWeaponStats st;
            check(bt_weapon_stats(&state, 0, &st) && st.cooldown > 0,
                  "slot 0 reports a live effective cooldown");
            float fresh = state.wpn_cd[0];
            check(near(fresh, (float)st.cooldown),
                  "slot 0's wave-begin draw is the exact basis (weapon_count == 0 bound)");
            check(near(obs[WEAPON_BASE + 3], 1.0f),
                  "the creation row reports the untouched fresh cooldown (ratio 1.0)");
            bt_core_step(&state, 0, obs, &rew, &term);   /* first arena frame */
            check(near(state.wpn_cd[0], fresh - 1.0f),
                  "the first arena frame decrements the fresh cooldown exactly once");
            check(near(obs[WEAPON_BASE + 3], (fresh - 1.0f) / (float)st.cooldown),
                  "the first arena row reports the one-decrement cooldown");
            check(state.tick_in_wave == 1, "the first arena frame is in-wave tick 1");
        }
    }

    std::printf("progression: %d failures\n", failures);
    return failures ? 1 : 0;
}
