/* Potential-based reward shaping for the Demon / endless objective.
 *
 * WHY THIS FILE EXISTS
 *   The primary objective is how deep a run gets: the game records a run as
 *   ProgressData's DifficultyScore (difficulty + wave) and, past wave 20, as
 *   `max_endless_wave_beaten.set_info(..., RunData.current_wave, ...)`
 *   (singletons/run_data.gd:1819, main.gd:944-953).  Log.perf now reports that
 *   wave directly (brotato_core.h bt_core_add_log), so the metric and the task
 *   agree.  The *reward* still has to carry credit back to the decisions that
 *   make deep waves possible - max HP, harvesting/luck, a banked-material
 *   buffer, the guide's item core and the weapon pivot - which are many
 *   thousands of ticks away from the wave bonus.  Rather than invent new
 *   incentives (which WOULD change the task), this file adds the standard
 *   potential-based shaping term
 *
 *       F(s, s') = gamma * Phi(s') - Phi(s)                (Ng et al. 1999)
 *
 *   for a hand-built Phi.  The Ng/Harada/Russell theorem is the whole point:
 *   for ANY Phi and gamma in [0,1] the optimal policy of the shaped MDP is
 *   identical to that of the original one, so this is pure credit assignment
 *   (variance/bias reduction along a known-good guide path), never a new
 *   objective.  Episodic convention: Phi(terminal) = 0, so the transition that
 *   ends the episode contributes -Phi(s).
 *
 * PHI GROUPS (term index == Log.rw[] index)
 *   0 hp        sqrt(max_hp)        Demon's whole defence is max HP, and its
 *                                   shop currency IS max_hp (run_data.gd:
 *                                   1428-1439).  sqrt, not log: both are
 *                                   concave, but sqrt is IEEE-correctly-rounded
 *                                   on host and device (bt_sqrtf == __fsqrt_rn)
 *                                   while device `logf` is not bit-identical to
 *                                   glibc's, which would break the CPU/CUDA/
 *                                   Metal reward parity the rest of the core
 *                                   maintains (see brotato_math_cuda.h).
 *   1 harvest   stat_harvesting     guide's early economy engine
 *   2 luck      stat_luck           guide's loot/box engine
 *   3 materials banked s->materials, only from `rw_late_wave` on (the guide's
 *                                   "lock the shop, bank materials late")
 *   4 items     the BT_RW_ITEM_IDX table below (Padding, Spicy Sauce, Garden,
 *               Improved Tools, Explosive Shells, Plastic Explosive, Alien
 *               Tongue, Hunting Trophy, Tardigrade, Anvil, Alien Eyes,
 *               Vigilante Ring, Grind's Magical Leaf, Silver Bullet, Bag,
 *               Recycling Machine), each with its own weight
 *   5 pivot     owned Pruner / Potato Thrower / Excalibur weapons, credit
 *               (tier + 1) per owned copy, so upgrading the pivot pays
 *
 * CONFIG
 *   Weights come from the [env] ini section via bt_reward_cfg_init() (all
 *   default 0 => `rew.on == 0` => F is exactly 0.0f and the reward stream is
 *   bit-identical to the unshaped one, so existing traces/tests are untouched).
 *   `rw_gamma` MUST equal [train] gamma: the theorem holds for the gamma the
 *   learner actually optimises.  The backend must call bt_reward_cfg_init()
 *   once per env after bt_core_init(); bt_core_reset() never touches `rew`.
 *
 * STAT IDS: core stat ids are the BT_STAT_NAMES order of brotato_upgrades.h:50
 * (id 0 is stat_max_hp -- it is also the hp_shop currency), so the three ids
 * below are spelled out here instead of depending on a name table that only
 * exists host-side.
 */
#pragma once

/* Fallback definitions: brotato_state.h owns these (struct Log is sized by
 * BT_RW_NTERMS), so a translation unit that somehow includes only this header
 * still compiles. */
#ifndef BT_RW_NTERMS
#define BT_RW_NTERMS 6
#endif
#ifndef BT_RW_NITEMS
#define BT_RW_NITEMS 16
#endif
#ifndef BT_RW_NPIVOT
#define BT_RW_NPIVOT 3
#endif

#define BT_RW_STAT_MAX_HP 0     /* BT_STAT_NAMES[0]  = "stat_max_hp" */
#define BT_RW_STAT_LUCK 10      /* BT_STAT_NAMES[10] = "stat_luck" */
#define BT_RW_STAT_HARVESTING 12 /* BT_STAT_NAMES[12] = "stat_harvesting" */

/* Phi group names, in term-index order (Log.rw[] and the harness export).
 * Host-only: the export in puf_log walks this array by index. */
BT_HOST_ONLY(static const char* const BT_RW_TERM_NAME[BT_RW_NTERMS] = {
    "rw_hp", "rw_harvest", "rw_luck", "rw_materials", "rw_items", "rw_pivot"
};)

/* Key items of the guide's Demon/endless build (High Endless + Spicy Sauce),
 * one row per counted item, with the matching ini key stem in
 * BT_RW_ITEM_KEY ("rw_" + stem is what bt_reward_cfg_init reads).  Both tables
 * are written out literally: an X-macro here would put a #define inside
 * BT_HOST_ONLY's argument list, which the preprocessor does not accept. */
BT_TABLE unsigned short BT_RW_ITEM_IDX[BT_RW_NITEMS] = {
    BT_ITEM_IDX_PADDING,            /* Padding               */
    BT_ITEM_IDX_SPICY_SAUCE,        /* Spicy Sauce           */
    BT_ITEM_IDX_GARDEN,             /* Garden                */
    BT_ITEM_IDX_IMPROVED_TOOLS,     /* Improved Tools        */
    BT_ITEM_IDX_EXPLOSIVE_SHELLS,   /* Explosive Shells      */
    BT_ITEM_IDX_PLASTIC_EXPLOSIVE,  /* Plastic Explosive     */
    BT_ITEM_IDX_ALIEN_TONGUE,       /* Alien Tongue          */
    BT_ITEM_IDX_HUNTING_TROPHY,     /* Hunting Trophy        */
    BT_ITEM_IDX_TARDIGRADE,         /* Tardigrade            */
    BT_ITEM_IDX_ANVIL,              /* Anvil                 */
    BT_ITEM_IDX_ALIEN_EYES,         /* Alien Eyes            */
    BT_ITEM_IDX_VIGILANTE_RING,     /* Vigilante Ring        */
    BT_ITEM_IDX_GRINDS_MAGICAL_LEAF,/* Grind's Magical Leaf  */
    BT_ITEM_IDX_SILVER_BULLET,      /* Silver Bullet         */
    BT_ITEM_IDX_BAG,                /* Bag                   */
    BT_ITEM_IDX_RECYCLING_MACHINE,  /* Recycling Machine     */
};

BT_HOST_ONLY(static const char* const BT_RW_ITEM_KEY[BT_RW_NITEMS] = {
    "item_padding", "item_spicy_sauce", "item_garden", "item_improved_tools",
    "item_explosive_shells", "item_plastic_explosive", "item_alien_tongue",
    "item_hunting_trophy", "item_tardigrade", "item_anvil", "item_alien_eyes",
    "item_vigilante_ring", "item_grinds_magical_leaf", "item_silver_bullet",
    "item_bag", "item_recycling_machine"
};)

/* BT_RW_NITEMS (brotato_state.h) must cover exactly these rows. */
#if defined(__cplusplus)
static_assert(BT_RW_NITEMS == 16, "BT_RW_NITEMS must match the BT_RW_ITEM tables");
#else
_Static_assert(BT_RW_NITEMS == 16, "BT_RW_NITEMS must match the BT_RW_ITEM tables");
#endif

/* ---------------------------------------------------------------------------
 * Phi.  Writes the per-group signed contribution of state `s` into `part`
 * (BT_RW_NTERMS floats) and returns their sum.  Pure function of the state:
 * no RNG, no draws, no writes to the simulation state.
 * ------------------------------------------------------------------------- */
BT_HD static float bt_reward_phi(BT_DEVICE BtState* s, BT_THREAD float* part) {
    for (int i = 0; i < BT_RW_NTERMS; i++) part[i] = 0.0f;
    if (!s->rew.on) return 0.0f;

    part[0] = s->rew.hp * bt_sqrtf(bt_maxf(0.0f, bt_core_stat_read(s, BT_RW_STAT_MAX_HP)));
    part[1] = s->rew.harvest * bt_core_stat_read(s, BT_RW_STAT_HARVESTING);
    part[2] = s->rew.luck * bt_core_stat_read(s, BT_RW_STAT_LUCK);
    if (s->wave >= s->rew.late_wave) part[3] = s->rew.materials * s->materials;

    float items = 0.0f;
    for (int k = 0; k < BT_RW_NITEMS; k++) {
        unsigned short idx = BT_RW_ITEM_IDX[k];
        if (idx >= BT_NITEMS || s->item_owned[idx] == 0) continue;
        float w = s->rew.item_w[k] != 0.0f ? s->rew.item_w[k] : s->rew.items;
        if (w != 0.0f) items += w * (float)s->item_owned[idx];
    }
    part[4] = items;

    /* Weapon pivot: an owned weapon whose FAMILY (lowest-tier row of its
     * BT_WEAPONS id group) is one of the configured pivot families.  Credit
     * (tier + 1) per copy so climbing the tiers is progress. */
    float pivot = 0.0f;
    for (int w = 0; w < BT_SLOTS; w++) {
        unsigned short id = s->wpn_id[w];
        if (id == BT_WEAPON_NONE || id >= BT_NWEAPONS) continue;
        unsigned short fam = BT_WEAPONS[id].family;
        for (int k = 0; k < BT_RW_NPIVOT; k++) {
            if (s->rew.pivot_family[k] == 0xFFFFu) continue;
            if (s->rew.pivot_family[k] == fam)
                pivot += (float)(BT_WEAPONS[id].tier + 1);
        }
    }
    part[5] = s->rew.pivot * pivot;

    float phi = 0.0f;
    for (int i = 0; i < BT_RW_NTERMS; i++) phi += part[i];
    return phi;
}

/* ---------------------------------------------------------------------------
 * F = gamma*Phi(s') - Phi(s) for one transition, plus the per-group ledger.
 * `p0` is the Phi decomposition captured at the START of the transition
 * (bt_core_step's first statement) and `terminal` says whether this transition
 * ends the episode (then Phi(s') = 0).  Returns the reward to add.
 * ------------------------------------------------------------------------- */
BT_HD static float bt_reward_shaping(BT_DEVICE BtState* s, const BT_THREAD float* p0,
                                     int terminal) {
    if (!s->rew.on) return 0.0f;
    float p1[BT_RW_NTERMS];
    if (!terminal) (void)bt_reward_phi(s, p1);
    if (terminal) for (int i = 0; i < BT_RW_NTERMS; i++) p1[i] = 0.0f;
    float f = 0.0f;
    for (int i = 0; i < BT_RW_NTERMS; i++) {
        float d = s->rew.gamma * p1[i] - p0[i];
        s->log.rw[i] += d;                 /* episode ledger, harness-averaged */
        f += d;
    }
    s->log.rw_total += f;
    return f;
}

/* Optional per-wave escalation of the existing R_WAVE bonus.  Off (weight 0)
 * by default and NOT potential-based: it deliberately pays deeper waves more,
 * which is aligned with the primary objective (endless depth) but does change
 * the incentive, so it must be switched on explicitly in the ini.
 *   bonus = R_WAVE * (1 + wave_scale * max(0, wave - 20) / 10) */
BT_HD static float bt_reward_wave_bonus(BT_DEVICE BtState* s) {
    float w = (float)s->wave;
    float extra = s->rew.wave_scale;
    if (extra <= 0.0f || s->wave <= BT_WAVES) return R_WAVE;
    return R_WAVE * (1.0f + extra * (w - (float)BT_WAVES) * 0.1f);
}

/* ---------------------------------------------------------------------------
 * Host configuration.  Zeroes every weight (shaping off, the default).
 * ------------------------------------------------------------------------- */
BT_HOST_ONLY(static void bt_reward_cfg_off(BT_DEVICE BtState* s) {
    s->rew.gamma = 0.99f;
    s->rew.hp = s->rew.harvest = s->rew.luck = 0.0f;
    s->rew.materials = s->rew.items = s->rew.pivot = 0.0f;
    s->rew.wave_scale = 0.0f;
    for (int k = 0; k < BT_RW_NITEMS; k++) s->rew.item_w[k] = 0.0f;
    for (int k = 0; k < BT_RW_NPIVOT; k++) s->rew.pivot_family[k] = 0xFFFFu;
    s->rew.late_wave = BT_WAVES;
    s->rew.on = 0;
})

/* Read the [env] section into s->rew.  Missing keys keep their default, so an
 * ini that sets nothing leaves the reward stream bit-identical to the base
 * game.  dict_get/dict_get_str exit(1) on a miss, so this uses dict_find.
 * The two build variants are separate definitions (no directive inside a macro
 * argument).  Harness builds (brotato.h / brotato.cu / puffercpu) include
 * src/pufferenv.h -> src/ini.h, where Dict is `typedef struct {...} Dict;` --
 * an anonymous struct, so `struct Dict` would be a DIFFERENT type under C++:
 * never forward-declare it, use the typedef.  Bare-core builds (tests) have no
 * Dict at all, so their stub takes void*. */
#ifdef PUF_BACKEND
#define BT_RW_NUM(field, key, def) \
    s->rew.field = ((it = dict_find(kv, key)) != NULL) ? (float)it->value : (def)
BT_HOST_ONLY(static inline void bt_reward_cfg_init(BT_DEVICE BtState* s, Dict* kv) {
    bt_reward_cfg_off(s);
    DictItem* it;
    BT_RW_NUM(gamma, "rw_gamma", 0.99f);
    BT_RW_NUM(hp, "rw_hp", 0.0f);
    BT_RW_NUM(harvest, "rw_harvest", 0.0f);
    BT_RW_NUM(luck, "rw_luck", 0.0f);
    BT_RW_NUM(materials, "rw_materials", 0.0f);
    BT_RW_NUM(items, "rw_items", 0.0f);
    BT_RW_NUM(pivot, "rw_pivot", 0.0f);
    BT_RW_NUM(wave_scale, "rw_wave_scale", 0.0f);
    it = dict_find(kv, "rw_late_wave");
    s->rew.late_wave = it ? (int)it->value : BT_WAVES;
    for (int k = 0; k < BT_RW_NITEMS; k++)
        s->rew.item_w[k] = ((it = dict_find(kv, BT_RW_ITEM_KEY[k])) != NULL)
            ? (float)it->value : 0.0f;
    /* Pivot weapon tokens -> the family row of that weapon id group. */
    for (int k = 0; k < BT_RW_NPIVOT; k++) {
        char key[16];
        snprintf(key, sizeof(key), "rw_pivot_%d", k + 1);
        it = dict_find(kv, key);
        if (it == NULL || it->str == NULL || it->str[0] == '\0') continue;
        int row = bt_weapon_index_from_id(it->str);
        if (row == (int)BT_WEAPON_NONE || row < 0 || row >= BT_NWEAPONS) continue;
        s->rew.pivot_family[k] = BT_WEAPONS[row].family;
    }
    /* Any non-zero weight turns shaping on; everything zero => exactly 0.0f. */
    int on = s->rew.hp != 0.0f || s->rew.harvest != 0.0f || s->rew.luck != 0.0f ||
             s->rew.materials != 0.0f || s->rew.items != 0.0f || s->rew.pivot != 0.0f;
    for (int k = 0; k < BT_RW_NITEMS; k++) if (s->rew.item_w[k] != 0.0f) on = 1;
    s->rew.on = on;
})
#undef BT_RW_NUM
#else
BT_HOST_ONLY(static inline void bt_reward_cfg_init(BT_DEVICE BtState* s, void* kv) {
    (void)kv;
    bt_reward_cfg_off(s);
})
#endif
