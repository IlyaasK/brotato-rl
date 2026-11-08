/* Brotato: source character profiles (Bull default, plus Knight/Demon) at
 * Danger 5, played for a full clear.
 *
 * Constants come from the game's own exported resources (Godot 3.7 pack,
 * v1.1.14.6):
 *   items/characters/bull/bull_effect_1..5.tres
 *       stat_max_hp +20, stat_hp_regeneration +15, stat_armor +10,
 *       effect_increase_stat_gains +50% on hp_regeneration,
 *       explode_on_hit chance 1.0, weapon_slot 0 (Bull carries no weapons)
 *   items/difficulties/5/difficulty_5_effect_0..5.tres
 *       danger_enemy_health +40%, danger_enemy_damage +40%,
 *       effect_elites, effect_more_elites, EFFECT_NEW_ENEMIES, double_boss
 *   zones/zone_1/001..020/wave_1..20.tres
 *       wave_duration 20,25,30,35,40,45,50,55,60 x11, 90
 *   entities/units/enemies/<kind>_stats.tres
 *       health 8 (+1.0/wave), damage 1 (+0.6/wave), speed 200, attack_cd 30f
 *   entities/units/player/player_stats.tres   speed 450
 *
 * REWARD DESIGN follows the community Bull guide rather than raw kills:
 *   - herding: a single explosion that catches N enemies pays
 *     N * (1 + 0.15*(N-1)), so clustering beats picking enemies off one by one
 *   - target selection: incoming damage is penalised per point taken, so walking
 *     into the heavy enemies is strictly worse than eating weak ones
 *   - regeneration use: a small per-step credit while HP sits below 85% of max,
 *     because the guide is explicit that sitting at full HP wastes regen
 *   - wave survival dominates: clearing a wave pays more than a whole wave of
 *     kills, and finishing wave 20 pays the largest term
 *   - speed matters implicitly: only the agent that can choose its collisions
 *     can farm these rewards without dying
 *
 * LAYOUT / PERFORMANCE
 *   Fixed-capacity state and persistent physics contacts, with no per-step
 *   allocation. CPU, CUDA and Metal step the same numerical implementation.
 *
 * SHARED CORE
 *   All simulation numerics live in brotato_core.h / brotato_state.h /
 *   brotato_rng.h and are stepped on BtState by CPU, CUDA and Metal alike. This
 *   header is only the HOST wrapper: raylib rendering plus the pufferenv
 *   Env/Agent/Client/Dict plumbing.
 *
 * Action space: one discrete head of BT_ACTIONS (0 = hold, 1..8 = 8-way
 * movement, 9..14 = recycle a weapon slot, 15..20 = combine two slots; the shop
 * and level-up phases reuse indices 0..4).
 *
 * Profile selector: puf_init reads `character` (canonical token, Bull by
 * default) and `starting_weapon` (canonical token, "none" by default) from the
 * env kwargs, resolves them on the host and passes both ids into bt_core_init,
 * identically to the CUDA and Metal backends.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "raylib.h"
typedef float obs_t;
#include "pufferenv.h"
#include "brotato_core.h"

#define BT_WINDOW 960
#define BT_HEIGHT 640

typedef struct { int frame; } Client;

/* Host wrapper of the portable BtState (brotato_state.h). Host-only members:
 * Agent buffers (pointers), the renderer Client and the harness-visible rng/tag
 * bookkeeping. `log` aliases state.log (both at offset 0) so the generic
 * harness's env.log access keeps working. */
typedef struct Env {
    union {
        BtState state;
        struct { struct Log log; };
    };
    Agent agents[1];
    Client* client;
    unsigned int rng;      /* host seed input; metal_env writes it before puf_init */
    int tag;
    int boundary_reached;
    int num_agents;
} Env;
typedef Env Brotato;

#define NUM_ATNS 1
#define ACT_SIZES {BT_ACTIONS}

/* Observation width is owned by the core schema (exactly BT_OBS_SIZE floats). */
#define OBS_SIZE BT_OBS_SIZE

/* nvcc parses this header in its DEVICE pass as well: build.sh's native/CPU
 * trainer compiles src/pufferl.cu with ENV_HEADER=ocean/brotato/brotato.h, and
 * nvcc parses the whole translation unit for the device too.  Everything below
 * is HOST-ONLY: the raylib renderer, the icon loaders and the Env/Dict
 * plumbing.  Those read generated tables whose `name`/`id`/`icon` fields and
 * name resolvers BT_HOST_ONLY erases in the device pass, so the device pass
 * fails with "no member named 'name'/'icon'/'id'" and undeclared resolvers.
 * The device pass needs only
 * Env/Log/obs_t plus the shared core, all declared above.  Same device-view
 * rule as ocean/brotato/brotato.cu; the host pass is bit-identical. */
#if !defined(__CUDA_ARCH__)

/* Host-only capacity diagnostic (stdio/abort): used by puf_reset/puf_step below,
 * so it must be included before them.  Not in the device pass -- the device
 * wrappers use bt_capacity_mask() from the freestanding brotato_capacity.h that
 * brotato_core.h already pulled in. */
#include "brotato_capacity_host.h"

/* Draw with the game's own art. Textures were extracted from the shipped
 * pack (STEX -> embedded WebP -> PNG) into ~/brotato_textures/png. */
static Texture2D bt_icon_cache[BT_NITEMS];
static signed char bt_icon_state[BT_NITEMS];

static Texture2D* bt_item_icon(unsigned short item) {
    if (item >= BT_NITEMS) return NULL;
    if (bt_icon_state[item] == 0) {
        const char* home = getenv("HOME");
        char path[512];
        snprintf(path, sizeof(path), "%s/brotato_textures/png/%s_icon.png",
                 home ? home : ".", BT_ITEMS[item].name);
        Texture2D t = LoadTexture(path);
        bt_icon_state[item] = t.id > 0 ? 1 : -1;
        bt_icon_cache[item] = t;
    }
    return bt_icon_state[item] == 1 ? &bt_icon_cache[item] : NULL;
}

static Texture2D bt_bull_tex;
static int bt_bull_state = 0;
static Texture2D* bt_bull_icon(void) {
    if (bt_bull_state == 0) {
        const char* home = getenv("HOME");
        char path[512];
        snprintf(path, sizeof(path), "%s/brotato_textures/png/bull_icon.png", home ? home : ".");
        bt_bull_tex = LoadTexture(path);
        bt_bull_state = bt_bull_tex.id > 0 ? 1 : -1;
    }
    return bt_bull_state == 1 ? &bt_bull_tex : NULL;
}

/* Equipped-weapon art, same extracted-texture contract as the item icons above.
 * BT_WEAPONS[id].icon is the host-only source res path
 * ("res://weapons/melee/hatchet/hatchet_icon.png"); the extractor writes the
 * basename to ~/brotato_textures/png, so only the last path segment is needed. */
static Texture2D bt_weapon_icon_cache[BT_NWEAPONS];
static signed char bt_weapon_icon_state[BT_NWEAPONS];
static Texture2D* bt_weapon_icon(unsigned short weapon) {
    if (weapon >= BT_NWEAPONS) return NULL;
    if (bt_weapon_icon_state[weapon] == 0) {
        const char* icon = BT_WEAPONS[weapon].icon;
        const char* slash = strrchr(icon, '/');
        const char* base = slash ? slash + 1 : icon;
        const char* home = getenv("HOME");
        char path[512];
        snprintf(path, sizeof(path), "%s/brotato_textures/png/%s", home ? home : ".", base);
        Texture2D t = LoadTexture(path);
        bt_weapon_icon_state[weapon] = t.id > 0 ? 1 : -1;
        bt_weapon_icon_cache[weapon] = t;
    }
    return bt_weapon_icon_state[weapon] == 1 ? &bt_weapon_icon_cache[weapon] : NULL;
}

/* Core stat id -> display name.  BT_STAT_NAMES is emitted by
 * extract_upgrades.py (index == core stat id, ids 0..BT_NSTATS-1), so the
 * renderer, the upgrade table and the GDScript bridge all read one contract
 * instead of three hand-copied lists.  Ids outside the contract (e.g. an item
 * effect whose key has no core stat) print "?" instead of wrapping into a
 * wrong name. */
#if BT_NUPGRADE_STAT_IDS != BT_NSTATS
#error "stat id contract drift: brotato_upgrades.h BT_NUPGRADE_STAT_IDS != brotato_state.h BT_NSTATS"
#endif

/* The on-screen stat legend, built from the emitted contract in thirds so it
 * always lists every core stat id (previously a hand-copied line that had gone
 * stale). */
#define BT_STAT_LEGEND_LINES 3
static const char* bt_stat_legend_line(int line) {
    static char lines[BT_STAT_LEGEND_LINES][512];
    static int built = 0;
    if (!built) {
        int per = (BT_NUPGRADE_STAT_IDS + BT_STAT_LEGEND_LINES - 1) / BT_STAT_LEGEND_LINES;
        for (int i = 0; i < BT_STAT_LEGEND_LINES; i++) lines[i][0] = '\0';
        for (int sid = 0; sid < BT_NUPGRADE_STAT_IDS; sid++) {
            int bucket = sid / per;
            if (bucket >= BT_STAT_LEGEND_LINES) bucket = BT_STAT_LEGEND_LINES - 1;
            char part[64];
            char* out = lines[bucket];
            snprintf(part, sizeof(part), "%s%d %s", out[0] == '\0' ? "stats: " : "  ",
                     sid, BT_STAT_NAMES[sid]);
            strncat(out, part, sizeof(lines[0]) - strlen(out) - 1);
        }
        built = 1;
    }
    if (line < 0 || line >= BT_STAT_LEGEND_LINES) return "";
    return lines[line];
}

static void brotato_human_controls(Brotato* env) {
    if (!IsWindowReady()) return;
    float* a = env->agents[0].actions;
    if (a == NULL) return;
    int dir = 0;
    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) dir |= 1;
    if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) dir |= 2;
    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) dir |= 4;
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) dir |= 8;
    static const signed char map[16] = {0,1,5,0,7,0,0,0,3,0,0,0,0,0,0,0};
    /* diagonal combinations resolved explicitly */
    int act = 0;
    switch (dir) {
    case 1: act = 1; break;  case 2: act = 5; break;
    case 4: act = 7; break;  case 8: act = 3; break;
    case 5: act = 4; break;  case 9: act = 2; break;
    case 6: act = 6; break;  case 10: act = 8; break;
    default: act = 0; break;
    }
    (void)map;
    a[0] = (float)act;
}

/* Host wrapper: all numerics delegate to the shared core. */
void add_log(Brotato* env) {
    bt_core_add_log(&env->state);
}

void puf_reset(Brotato* env) {
    bt_core_reset(&env->state);
    bt_core_observe(&env->state, env->agents[0].observations);
    bt_action_mask(&env->state, env->agents[0].action_mask);
    bt_capacity_require_clean(&env->state, "puf_reset");
}

void puf_step(Brotato* env) {
    float* actions = env->agents[0].actions;
    int action = actions ? (int)actions[0] : 0;
    bt_core_step(&env->state, action, env->agents[0].observations,
                 env->agents[0].rewards, env->agents[0].terminals);
    /* Publish the phase's legal ids for the next decision (the trainer owns the
     * buffer and assigns agents[0].action_mask after puf_init). */
    bt_action_mask(&env->state, env->agents[0].action_mask);
    /* Fail closed BEFORE the trainer consumes this transition: a state that lost
     * an entity/loot to an implementation bound is not a valid rollout. */
    bt_capacity_require_clean(&env->state, "puf_step");
}

void puf_render(Brotato* env) {
    BtState* s = &env->state;
    if (!IsWindowReady()) {
        InitWindow(BT_WINDOW, BT_HEIGHT, "Brotato - Bull (PufferLib)");
        SetTargetFPS(60);
        env->client = (Client*)calloc(1, sizeof(Client));
    }
    if (IsKeyDown(KEY_ESCAPE)) exit(0);
    brotato_human_controls(env);

    const float sc = fminf((float)BT_WINDOW / BT_ARENA_W, (float)BT_HEIGHT / BT_ARENA_H);
    BeginDrawing();
    ClearBackground((Color){26, 30, 24, 255});
    for (int gx = 0; gx < (int)BT_ARENA_W; gx += 80)
        DrawLine((int)(gx * sc), 0, (int)(gx * sc), BT_HEIGHT, (Color){34, 40, 32, 255});
    for (int gy = 0; gy < (int)BT_ARENA_H; gy += 80)
        DrawLine(0, (int)(gy * sc), BT_WINDOW, (int)(gy * sc), (Color){34, 40, 32, 255});

    for (int i = 0; i < BT_MAX; i++) {
        if (!s->ealive[i]) continue;
        Color c = s->eelite[i] == 2 ? (Color){220, 65, 80, 255}
                : s->eelite[i] == 1 ? (Color){240, 120, 60, 255}
                                   : (Color){140, 110, 120, 255};
        const BtBodyPhysics* geometry = bt_kgeom(s, i);
        DrawCircle((int)((s->ex[i] + geometry->body_x) * sc),
                   (int)((s->ey[i] + geometry->body_y) * sc),
                   geometry->body_radius * sc, c);
    }
    /* Projectile surfaces. Friendly shots (player/pet/turret) gold, enemy shots
     * red; geometry mirrors bt_projectile_hits (oriented rect for SLASH/PILLAR,
     * circle otherwise) so the drawn footprint is the collision footprint. */
    for (int i = 0; i < BT_MAX_PROJ; i++) {
        if (!s->pralive[i]) continue;
        int cx = (int)(s->prx[i] * sc), cy = (int)(s->pry[i] * sc);
        Color c = s->prfriendly[i] ? (Color){250, 220, 120, 255}
                                   : (Color){230, 110, 130, 255};
        float prot = s->prrot[i];
        float deg = prot * 57.2957795f;
        if (s->prshape[i] == BT_SHAPE_SLASH) {
            float ox = cosf(prot) * BT_SLASH_OFF_X - sinf(prot) * BT_SLASH_OFF_Y;
            float oy = sinf(prot) * BT_SLASH_OFF_X + cosf(prot) * BT_SLASH_OFF_Y;
            float hx = s->prhx[i], hy = s->prhy[i];
            DrawRectanglePro((Rectangle){(float)cx + ox, (float)cy + oy, 2.0f * hx, 2.0f * hy},
                             (Vector2){hx, hy}, deg, c);
        } else if (s->prshape[i] == BT_SHAPE_PILLAR) {
            float ox = cosf(prot) * BT_PILLAR_OFF_X - sinf(prot) * BT_PILLAR_OFF_Y;
            float oy = sinf(prot) * BT_PILLAR_OFF_X + cosf(prot) * BT_PILLAR_OFF_Y;
            float r = s->prrad[i];
            DrawRectanglePro((Rectangle){(float)cx + ox, (float)cy + oy, 2.0f * r, 2.0f * s->prhy[i]},
                             (Vector2){r, s->prhy[i]}, deg, c);
        } else {
            DrawCircle(cx, cy, s->prrad[i] > 0.0f ? s->prrad[i] * sc : 4.0f, c);
        }
    }
    {
        Texture2D* bt = bt_bull_icon();
        int cx = (int)(s->px * sc), cy = (int)(s->py * sc);
        if (bt) {
            float sz = 44.0f;
            Rectangle src = {0, 0, (float)bt->width, (float)bt->height};
            Rectangle dst = {cx - sz * 0.5f, cy - sz * 0.5f, sz, sz};
            DrawTexturePro(*bt, src, dst, (Vector2){0, 0}, 0.0f, WHITE);
        } else {
            DrawCircle(cx, cy, 14, (Color){235, 210, 120, 255});
        }
    }
    DrawCircleLines((int)(s->px * sc), (int)((s->py + BT_EXPLODE_OFF_Y) * sc), (int)(BT_EXPLODE_R * sc),
                    (Color){235, 210, 120, 40});

    /* Equipped weapons: real extracted art along a bottom slot strip, a cooldown
     * bar per slot, and the melee swing surface (exact source tween geometry via
     * bt_melee_visual) highlighted while the hit window is open. */
    for (int i = 0; i < (int)s->wpn_count && i < BT_SLOTS; i++) {
        unsigned short wid = s->wpn_id[i];
        if (s->wpn_melee_shooting[i]) {
            float ox = 0.0f, oy = 0.0f, rot = 0.0f;
            bt_melee_visual(s, i, &ox, &oy, &rot);
            float sx = (float)(s->px + ox) * sc, sy = (float)(s->py + oy) * sc;
            Color wc = s->wpn_melee_hit_on[i] ? (Color){255, 240, 160, 220}
                                              : (Color){200, 200, 210, 120};
            DrawRectanglePro((Rectangle){sx, sy, 34.0f, 6.0f}, (Vector2){0, 3},
                             rot * 57.2957795f, wc);
        }
        int bx = BT_WINDOW - 44 - i * 40, by = BT_HEIGHT - 40;
        Texture2D* ic = bt_weapon_icon(wid);
        if (ic) {
            Rectangle src = {0, 0, (float)ic->width, (float)ic->height};
            Rectangle dst = {(float)(bx - 15), (float)(by - 15), 30.0f, 30.0f};
            DrawTexturePro(*ic, src, dst, (Vector2){0, 0}, 0.0f, WHITE);
        }
        DrawRectangleLines(bx - 16, by - 16, 32, 32,
                           s->wpn_melee_hit_on[i] ? (Color){255, 240, 160, 255}
                                                  : (Color){140, 150, 170, 255});
        /* Cooldown bar (wpn_cd is in TICKS; the source cooldown is <= 60*seconds,
         * so clamp the fill to the slot box). */
        float fill = s->wpn_cd[i] / 60.0f;
        if (fill < 0.0f) fill = 0.0f;
        if (fill > 1.0f) fill = 1.0f;
        DrawRectangle(bx - 16, by + 17, (int)(32.0f * fill), 3, (Color){240, 200, 120, 255});
    }

    DrawText(TextFormat("wave %d/%d", s->wave, BT_WAVES), 12, 10, 24, RAYWHITE);
    const char* char_name = (s->character < (unsigned short)BT_NCHARACTERS)
                                ? BT_CHARACTERS[s->character].name : "?";
    DrawText(TextFormat("%.0fs  %s", s->time_left, char_name), 12, 38, 20, GRAY);
    DrawText(TextFormat("kills %d", s->kills), 200, 10, 24, (Color){240, 200, 120, 255});
    DrawText(TextFormat("hp %.0f/%.0f", s->hp, s->max_hp), 200, 38, 20,
             s->hp < s->max_hp * 0.35f ? (Color){240, 90, 90, 255} : RAYWHITE);
    DrawText(TextFormat("regen %.0f  armor %.0f", s->regen, s->armor), 420, 10, 20,
             (Color){150, 200, 160, 255});
    DrawText(TextFormat("enemies %d", s->ecount), 420, 34, 20, (Color){180, 150, 150, 255});

    /* materials + progression HUD */
    DrawText(TextFormat("materials %.0f", s->materials), 640, 10, 20, (Color){120, 220, 160, 255});
    DrawText(TextFormat("level %d   dmg%% %.0f   armor %.0f", s->level, s->dmg_pct, s->armor),
             640, 34, 18, (Color){160, 200, 220, 255});
    DrawText(TextFormat("dodge %.0f%%  crit %.0f%%  speed%% %.0f", s->dodge * 100.0f,
                        s->crit * 100.0f, s->speed_pct),
             640, 56, 16, (Color){150, 170, 200, 255});

    /* SHOP / LEVEL-UP overlay: without this the phases were invisible in eval */
    if (s->phase == 1 || s->phase == 2) {
        int px0 = 120, py0 = 150, pw = BT_WINDOW - 240, ph = 380;
        DrawRectangle(px0, py0, pw, ph, (Color){18, 20, 26, 240});
        DrawRectangleLines(px0, py0, pw, ph, (Color){120, 160, 200, 255});
        const char* title = s->phase == 1 ? "SHOP  -  choose 0..3, 4 reroll, 8 skip"
                                          : "LEVEL UP  -  choose 0..3";
        DrawText(title, px0 + 20, py0 + 14, 22, (Color){235, 220, 140, 255});
        DrawText(TextFormat("wave %d   materials %.0f   level %d", s->wave, s->materials, s->level),
                 px0 + 20, py0 + 44, 18, RAYWHITE);
        for (int i = 0; i < 4; i++) {
            int y = py0 + 84 + i * 64;
            if (s->phase == 1) {
                /* Canonical offer identity is shop_kind[i] + shop_id[i]: an ITEM
                 * is a BT_ITEMS row, a WEAPON a BT_WEAPONS row, and a NONE slot
                 * (empty or already bought) carries no offer, so it is left
                 * blank instead of rendering the old item[0] placeholder. Price
                 * and affordability come from the shop's own helpers, which fold
                 * in the run's currency (Demon pays max HP) and profile gates. */
                unsigned char kind = s->shop_kind[i];
                unsigned short id = s->shop_id[i];
                const char* unit = bt_wshop_hp_shop(s) ? "hp" : "mat";
                Texture2D* ic = NULL;
                float price = 0.0f;
                int afford = 0;
                char label[128];
                if (kind == BT_SHOP_ITEM) {
                    const BtItem* it = &BT_ITEMS[id];
                    price = bt_shop_slot_price(s, i);
                    afford = bt_wshop_currency(s) >= price;
                    ic = bt_item_icon(id);
                    snprintf(label, sizeof(label), "[%d] %-18s T%d  %.0f %s%s", i, it->name,
                             it->tier, price, unit, s->shop_cursed[i] ? "  cursed" : "");
                } else if (kind == BT_SHOP_WEAPON) {
                    const BtWeaponDef* wp = &BT_WEAPONS[id];
                    price = bt_shop_slot_price(s, i);
                    afford = bt_wshop_can_buy(s, id) && bt_wshop_currency(s) >= price;
                    ic = bt_weapon_icon(id);
                    snprintf(label, sizeof(label), "[%d] %-18s T%d %s  %.0f %s%s", i, wp->id,
                             wp->tier, wp->kind == BT_WEAPON_KIND_MELEE ? "melee" : "ranged",
                             price, unit, s->shop_cursed[i] ? "  cursed" : "");
                } else {
                    snprintf(label, sizeof(label), "[%d] --", i);
                }
                if (ic) {
                    Rectangle src = {0, 0, (float)ic->width, (float)ic->height};
                    Rectangle dst = {(float)(px0 + 26), (float)(y - 6), 40, 40};
                    DrawTexturePro(*ic, src, dst, (Vector2){0, 0}, 0.0f,
                                   afford ? WHITE : (Color){110, 110, 110, 255});
                }
                DrawText(label, px0 + 76, y, 20,
                         afford ? RAYWHITE : (Color){110, 110, 120, 255});
                if (kind == BT_SHOP_ITEM) {
                    const BtItem* it = &BT_ITEMS[id];
                    for (int e = 0; e < it->count; e++) {
                        const BtItemEffect* ef = &BT_ITEM_EFFECTS[it->first + e];
                        DrawText(TextFormat("%s %+.0f", bt_stat_display_name(ef->stat), ef->value),
                                 px0 + 430 + e * 110, y, 18, (Color){150, 210, 170, 255});
                    }
                } else if (kind == BT_SHOP_WEAPON) {
                    const BtWeaponDef* wp = &BT_WEAPONS[id];
                    DrawText(TextFormat("dmg %d  cd %d", wp->damage, wp->cooldown),
                             px0 + 76, y + 22, 16, (Color){150, 210, 170, 255});
                }
            } else {
                /* The weapon-slot sentinel offer is drawn as its implemented
                 * value; a genuine BT_UPGRADES row decodes to row+1. */
                int row = bt_lvl_offer_row(s->lvl_offers[i]);
                const BtUpgrade* up = row < 0 ? &BT_UPGRADES[0] : &BT_UPGRADES[row];
                DrawText(TextFormat("[%d] %s  +%.0f  (tier %d)", i, bt_stat_display_name(up->stat),
                                    up->value, up->tier), px0 + 30, y, 20, RAYWHITE);
            }
        }
        for (int line = 0; line < BT_STAT_LEGEND_LINES; line++)
            DrawText(bt_stat_legend_line(line), px0 + 20, py0 + ph - 60 + line * 18, 13,
                     (Color){140, 150, 165, 255});
    }
    EndDrawing();
    puf_web_vsync();
}

void puf_close(Brotato* env) {
    if (env->client != NULL) { free(env->client); env->client = NULL; }
    if (IsWindowReady()) CloseWindow();
}

void puf_log(Log* log, Dict* out) {
    dict_set(out, "perf", log->perf);
    dict_set(out, "score", log->score);
    dict_set(out, "episode_return", log->episode_return);
    dict_set(out, "episode_length", log->episode_length);
    dict_set(out, "n", log->n);
    /* Endless mode: the highest wave the run reached (0 for a campaign run). */
    dict_set(out, "endless_wave", log->endless_wave);
    /* Potential-based shaping ledger (brotato_reward.h): one key per Phi group
     * plus their sum, so a run shows both the total credit the shaping added and
     * which guide group produced it. All zero when shaping is off. */
    dict_set(out, "rw_total", log->rw_total);
    for (int i = 0; i < BT_RW_NTERMS; i++) dict_set(out, BT_RW_TERM_NAME[i], log->rw[i]);
}

void puf_init(Env* env, Dict* kwargs) {
    env->num_agents = 1;
    env->client = NULL;
    env->agents[0].action_mask = NULL;
    env->agents[0].policy = 0;
    int shop_steps = (int)dict_get(kwargs, "shop_steps");
    int zone = (int)dict_get(kwargs, "zone");
    /* Profile selector: the ini carries canonical tokens ("bull", "none"); the
     * host resolves them to ids once, exactly like the GPU backends do, so all
     * three backends enter the same bt_core_init profile. */
    unsigned short character = bt_character_id_from_name(dict_get_str(kwargs, "character"));
    unsigned short starting_weapon = (unsigned short)bt_weapon_id_from_name(dict_get_str(kwargs, "starting_weapon"));
    /* Audio-RNG model, an OPTIONAL [env] key (dict_find, like `endless`):
     * default 1 keeps the legacy behaviour (the sim consumes the source's sound
     * draws); 0 selects the bridge's audiovisual neutralization, where the sound
     * queues/arrays are cleared and exactly those draws do not happen. */
    int audio_rng = 1;
    {
        DictItem* it_audio = dict_find(kwargs, "audio_rng");
        if (it_audio) audio_rng = (it_audio->value != 0.0) ? 1 : 0;
    }
    /* Particle-RNG model, same OPTIONAL [env] form: 1 (default) keeps the
     * source's CPUParticles2D draws, 0 selects the bridge's suppression. */
    int particles_rng = 1;
    {
        DictItem* it_part = dict_find(kwargs, "particles_rng");
        if (it_part) particles_rng = (it_part->value != 0.0) ? 1 : 0;
    }
    /* DebugService.invulnerable (run_data.gd:556), an OPTIONAL [env] key
     * (dict_find, like `endless`): default 0 keeps the normal player hurtbox; 1
     * disables it exactly as player.gd:114-115 does, so no contact/projectile
     * hit on the player is registered (see BtState.invulnerable). */
    int invulnerable = 0;
    {
        DictItem* it_inv = dict_find(kwargs, "invulnerable");
        if (it_inv) invulnerable = (it_inv->value != 0.0) ? 1 : 0;
    }
    /* Seed input: the harness (metal_env) writes env->rng before puf_init. Keep
     * the historical fallback exactly so env i matches the CPU trainer. */
    if (env->rng == 0) env->rng = 1u + (unsigned int)env->tag;
    bt_core_init(&env->state, (bt_u64)env->rng, zone, shop_steps, character, starting_weapon,
                 (unsigned char)audio_rng);
    /* Set after bt_core_init (whose arity stays unchanged); bt_core_reset
     * preserves it, and the prologue draws no particle randomness. */
    env->state.particles_rng = (unsigned char)(particles_rng ? 1 : 0);
    /* Same "after bt_core_init" placement and same reset-preserved contract as
     * particles_rng (bt_core_init defaults it to 0; bt_core_reset leaves it). */
    env->state.invulnerable = (unsigned char)(invulnerable ? 1 : 0);
    /* Endless-mode run option (RunData.is_endless_run), an OPTIONAL [env] key
     * (dict_find, like unlock_all below): a config without `endless` stays
     * campaign. Set AFTER bt_core_init (which defaults it to 0); bt_core_reset
     * never clears it, so it persists across episodes. */
    {
        DictItem* it_endless = dict_find(kwargs, "endless");
        env->state.endless = (unsigned char)((it_endless && it_endless->value != 0.0) ? 1 : 0);
    }
    /* Starting wave (RunData.reset() `current_wave = DebugService.starting_wave`,
     * run_data.gd:549), an OPTIONAL [env] key like `endless`: a config without
     * `start_wave` keeps the bt_core_init default of 1 (the campaign boot), so an
     * ini that sets nothing is byte-identical.  Set AFTER bt_core_init;
     * bt_core_reset never clears it, so it persists across episodes. */
    {
        DictItem* it_start_wave = dict_find(kwargs, "start_wave");
        if (it_start_wave != NULL && it_start_wave->value >= 1.0) {
            env->state.start_wave = (unsigned short)it_start_wave->value;
        }
    }
    /* Loot pool unlock profile (progress_data.gd:884-895): the ini's
     * `unlock_all` defaults to 1 = fully unlocked (the guide's setup); a missing
     * key keeps the bt_core_init default, so an ini that sets nothing is
     * unchanged.  unlock_all = 0 models a fresh save. */
    DictItem* it_unlock = dict_find(kwargs, "unlock_all");
    if (it_unlock != NULL)
        env->state.lock_default_items = (unsigned char)((int)it_unlock->value ? 0 : 1);
    /* Optional potential-based shaping weights from the [env] ini section
     * (brotato_reward.h). Read AFTER bt_core_init: init must not clobber the
     * configured weights, and reset never touches them. */
    bt_reward_cfg_init(&env->state, kwargs);
    bt_core_observe(&env->state, env->agents[0].observations);
    /* Fail closed: a configuration whose very first state already lost work to an
     * implementation bound must not be trained on. */
    bt_capacity_require_clean(&env->state, "puf_init");
}

#endif /* !defined(__CUDA_ARCH__): CPU host wrapper (raylib renderer + Env plumbing) */
