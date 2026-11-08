/* brotato_combat.h -- combat metadata the generated gameplay tables do not
 * carry: animation-driven projectile-shape windows, per-scene boost/spawn
 * constants, and the boss state clock period.
 *
 * Owner: SourceGameplay (core/state slice). Every constant is transcribed from
 * the recovered Brotato v1.1.14.6 scenes/scripts under
 * /Users/leelachebiyyam/brotato_recovered with file:line provenance. Anything
 * that can live in the generated tables is requested from GameplayTables
 * (see the buffer/spawner note at the bottom) so this file stays minimal.
 *
 * Pure data + POD: no pointers, no allocation, MSL/CUDA-safe.
 */
#pragma once
#include "brotato_rng.h"   /* BT_HD (device-portability prelude) */

/* --- projectile shapes (BT_ATTACKS[].proj_shape) --- */
#define BT_SHAPE_CIRCLE 0
#define BT_SHAPE_PILLAR 1
#define BT_SHAPE_SLASH  2
#define BT_SHAPE_OTHER  3
/* slash_projectile_2.tscn: the same shoot keyframes (BT_SLASH_SWEEPS row 1) but
 * the Hitbox node sits at (165,-15) instead of (0,-15), so the sweep origin
 * differs and the shape code carries that (tentacle, mom). */
#define BT_SHAPE_SLASH2 4

/* The animated Hitbox/Collision sweep of a slash projectile at animation
 * position `pos` (seconds, AnimationPlayer.current_animation_position for
 * anims/shoot, which autoplays).  Returns the shape CENTRE in the projectile
 * root's local frame (Hitbox node position + Collision position) and the
 * constant half extents.
 *
 * The position value track is interp=1 (LINEAR), update=0 (CONTINUOUS), so the
 * value is the linear interpolation of the generated keyframes: before the first
 * key the shape keeps the scene's rest position, after the last key the last
 * value.  The scene's `Hitbox/Collision:shape:extents` track is INERT in the
 * pinned engine (brotato_attacks.h has the measurement), so ext_x/ext_y are the
 * CONSTANT half extents of the scene's RectangleShape2D. */
BT_HD static void bt_slash_shape(int shape, float pos,
                                 BT_THREAD float* cx, BT_THREAD float* cy,
                                 BT_THREAD float* hx, BT_THREAD float* hy) {
    int row = (shape == BT_SHAPE_SLASH2) ? 1 : 0;
    if (row >= BT_NSWEEPS) row = 0;
    BT_CONST_PTR(BtSlashSweep) S = &BT_SLASH_SWEEPS[row];
    float x = S->rest_x, y = S->rest_y;
    if (BT_SWEEP_KEYS > 0 && pos >= BT_SWEEP_KEY_TIME[0]) {
        int k = BT_SWEEP_KEYS - 1;
        for (int i = BT_SWEEP_KEYS - 1; i >= 0; i--) if (pos >= BT_SWEEP_KEY_TIME[i]) { k = i; break; }
        if (k >= BT_SWEEP_KEYS - 1) {
            x = BT_SWEEP_KEY_X[BT_SWEEP_KEYS - 1];
            y = BT_SWEEP_KEY_Y[BT_SWEEP_KEYS - 1];
        } else {
            float t0 = BT_SWEEP_KEY_TIME[k], t1 = BT_SWEEP_KEY_TIME[k + 1];
            float u = (t1 > t0) ? (pos - t0) / (t1 - t0) : 0.0f;
            x = BT_SWEEP_KEY_X[k] + (BT_SWEEP_KEY_X[k + 1] - BT_SWEEP_KEY_X[k]) * u;
            y = BT_SWEEP_KEY_Y[k] + (BT_SWEEP_KEY_Y[k + 1] - BT_SWEEP_KEY_Y[k]) * u;
        }
    }
    *cx = S->node_x + x;
    *cy = S->node_y + y;
    *hx = S->ext_x;
    *hy = S->ext_y;
}

/* enemy_projectile.tscn Hitbox/Collision CircleShape2D radius 17.0294 @ (0,0)
 * (projectiles/bullet_enemy/enemy_projectile.tscn:7-23). */
#define BT_PROJ_R_DEFAULT 17.0294f

/* pillar_projectile.tscn: Hitbox circle radius 23 @ (0,-4); its method track
 * disables the hitbox at t=0, enables it at 0.54, disables at 0.68 and stops
 * at 1.04 (projectiles/pillar_enemy/pillar_projectile.tscn:62-77; radius
 * 10-11). Radius comes from the table (proj_radius); destroy_on_leaving_screen
 * is the Projectile default true. */
#define BT_PILLAR_OFF_X 0.0f
#define BT_PILLAR_OFF_Y (-4.0f)
#define BT_PILLAR_ON 0.54f
#define BT_PILLAR_OFF 0.68f
#define BT_PILLAR_STOP 1.04f

/* slash_projectile.tscn: Hitbox offset (0,-15) (:153); method track enables the
 * hitbox at 0.48, disables at 0.60 and stops at 0.63 (:90-91); extents come
 * from the table (proj_half_x/proj_half_y, keyframe-animated 16x9 .. 136.5x14);
 * destroy_on_leaving_screen = false (:146). */
#define BT_SLASH_OFF_X 0.0f
#define BT_SLASH_OFF_Y (-15.0f)
#define BT_SLASH_ON 0.48f
#define BT_SLASH_OFF 0.60f
#define BT_SLASH_STOP 0.63f

/* --- projectile despawn bounds: ZoneService.current_zone_max_camera_rect ----
 * projectile.gd:43-44
 *   if destroy_on_leaving_screen and not ZoneService.current_zone_max_camera_rect.has_point(position):
 *       stop()
 * The rect is written once per wave by main.gd:198-201
 *   var max_bounds = ZoneService.get_current_zone_rect().grow_individual(EDGE_SIZE, EDGE_SIZE * 2, EDGE_SIZE, EDGE_SIZE)
 *   _camera.init(max_bounds, float(EDGE_SIZE))
 *   ZoneService.current_zone_max_camera_rect = _camera.get_max_camera_bounds()
 * with EDGE_SIZE = 96 (main.gd:20) and get_max_camera_bounds (my_camera.gd:229-241)
 * returning _max_bounds.grow_individual(x_grow_by / 2, y_grow_by / 2, ...) where
 * the non-coop grow is max(0, Utils.project_width - size.x) / max(0,
 * Utils.project_height - size.y).  For the campaign zone (zone_1_data.tres:60-61
 * width 32, height 24 -> current_zone_rect = Rect2(0,0,2048,1536),
 * Utils.TILE_SIZE 64) that is Rect2(-96,-192,2240,1824); the project window
 * (project.godot display/window/size 1920x1080) is smaller on both axes, so the
 * grow is zero and the rect is exact.  Rect2.has_point is half-open:
 * x0 <= p.x < x0+size.x, y0 <= p.y < y0+size.y. */
#define BT_CAM_RECT_X0 (-96.0f)
#define BT_CAM_RECT_Y0 (-192.0f)
#define BT_CAM_RECT_X1 (BT_CAM_RECT_X0 + 2240.0f)
#define BT_CAM_RECT_Y1 (BT_CAM_RECT_Y0 + 1824.0f)

BT_HD static int bt_cam_rect_has_point(float x, float y) {
    return x >= BT_CAM_RECT_X0 && x < BT_CAM_RECT_X1 &&
           y >= BT_CAM_RECT_Y0 && y < BT_CAM_RECT_Y1;
}

/* --- boss state clock (entities/units/enemies/boss/boss.gd:92-101,
 * boss.tscn:32-34) --- CheckStateTimer autostart with the Timer default
 * wait_time 1.0 s; boss.gd increments elapsed_time by 1 per timeout, so state
 * activation (health <= hp_start*max OR elapsed_time >= timer_start) is on a
 * 1 Hz clock, not continuous. */
#define BT_BOSS_STATE_PERIOD 1.0f

/* --- enemy decaying slow on dealing contact damage (enemy.gd:321-322) ---
 * add_decaying_speed(-max(200, current_speed*0.8)) when the enemy hitbox hits. */
#define BT_ENEMY_HIT_DECAY_MIN 200.0f
#define BT_ENEMY_HIT_DECAY_PCT 0.8f

/* Boost (buffer) and death-spawn (spawner/blobfish) metadata are emitted by the
 * generator as data: BT_KIND_BOOST[BT_NKINDS] (gate on radius > 0) and
 * BT_KIND_DEATH_SPAWN_KIND/NB/RADIUS[BT_NKINDS][BT_MAX_DEATH_SPAWNS] +
 * BT_KIND_NDEATH_SPAWNS[BT_NKINDS]. Nothing manual remains here. */

