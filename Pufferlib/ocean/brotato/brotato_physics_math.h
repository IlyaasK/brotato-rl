/* brotato_physics_math.h -- scalar physics math for the Brotato Bull/Danger-5
 * emulator, ported from the recovered Godot 3.x game scripts.
 *
 * SCOPE / PROVENANCE
 *   Every function below is a direct transcription of a formula in the
 *   decompiled campaign scripts under /Users/leelachebiyyam/brotato_recovered:
 *
 *     entities/units/unit/unit.gd
 *       :136-162  _physics_process  (movement -> cached velocity, kinematic tests)
 *       :164-212  _integrate_forces (origin clamp, then target projection/snap)
 *       :214-224  get_movement / get_move_input / get_move_speed
 *       :226-241  get_next_velocity / get_next_knockback_value / get_knockback_value
 *       :265-348  take_damage      (stored replacement, .2/.8 direction, piercing,
 *                                   death knockback)
 *       :402-438  get_knockback_amount_based_on_distance_to_attacker (pull)
 *       :445-448  get_increased_piercing_knockback_amount
 *       :730-741  add_decaying_speed
 *
 *   Engine-side arithmetic (Vector2.normalized/project/linear_interpolate/
 *   direction_to, Rect2.has_point, Math::clamp, and the circle/circle overlap
 *   predicate used by discrete Area2D monitoring) is transcribed from:
 *
 *     core/math/vector2.h, core/math/vector2.cpp          (Godot Engine 3.x)
 *     core/math/rect2.h
 *     servers/physics_2d/collision_solver_2d_sat.cpp      (_collision_circle_circle)
 *     servers/physics_2d/area_pair_2d_sw.cpp              (AreaPair2DSW::setup)
 *     servers/physics_2d/collision_object_2d_sw.h         (collision filter)
 *
 *   Godot Engine is MIT-licensed; the transcribed arithmetic is redistributed
 *   under the following notice:
 *
 *     Copyright (c) 2014-present Godot Engine contributors
 *     Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur
 *
 *     Permission is hereby granted, free of charge, to any person obtaining a
 *     copy of this software and associated documentation files (the
 *     "Software"), to deal in the Software without restriction, including
 *     without limitation the rights to use, copy, modify, merge, publish,
 *     distribute, sublicense, and/or sell copies of the Software, and to
 *     permit persons to whom the Software is furnished to do so, subject to
 *     the following conditions:
 *
 *     The above copyright notice and this permission notice shall be included
 *     in all copies or substantial portions of the Software.
 *
 *     THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *     OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *     MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *     IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 *     CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 *     TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 *     SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * DESIGN CONSTRAINTS
 *   - Pure by-value scalar math. No allocation, no raylib, no Env/Brotato
 *     pointer, no host-only state. Callers supply every input as a parameter,
 *     so the same functions run in the CPU env, in a CUDA kernel, or in a
 *     Metal shader (see BT_HD / BT_SQRTF below; brotato.h may predefine them).
 *   - Float arithmetic follows the game exactly, including where it is NOT the
 *     "mathematically clean" form: the weighted knockback direction is never
 *     renormalised, the stored knockback vector is {replaced, not accumulated}
 *     and decayed by a fixed 0.9 per callback (not delta-scaled), the
 *     resistance factor is (100 - resistance * 100) and not 100 * (1 -
 *     resistance), and Vector2 division is a per-component divide rather than a
 *     multiply-by-reciprocal.
 *   - Vector2 comparison is Godot 3.x exact component equality; because
 *     operator== is exact, "_integrate_forces_velocity == Vector2.ZERO" is an
 *     exact-zero test (bt_v2eq).
 *
 * EXPLICIT NON-GOALS (stated, not stubbed -- there is no placeholder code for
 * these anywhere in this header):
 *   - The native solver. Enemy/enemy body separation, contact caching, warm
 *     started impulses, penetration bias, solver iterations and the
 *     kinematic `test_motion` slide loop are GodotPhysics internals; this
 *     header does not model body separation or claim solver parity.
 *   - Swept tests. Every overlap predicate here is a discrete test at the
 *     current transforms, matching Area2D monitoring, which re-solves once per
 *     physics step with zero margin.
 *   - `set_hitbox_damage_modifier()` (unit.gd:423-424): the Unit base method is
 *     an empty `pass`; enemy.gd overrides it with a 1 s damage reduction that
 *     mutates hitbox state. It is a host-side bookkeeping side effect outside
 *     this formula and is not modelled here.
 *   - The `speed_removed` signal emitted by add_decaying_speed for negative
 *     values: signalling only, no effect on the returned value.
 *   - `_can_move` gating of the movement vector (unit.gd:137) is the caller's
 *     job: pass Vector2.ZERO as `movement` when the unit may not move.
 *   - Player damage. player.gd:412-535 has its own take_damage that never
 *     assigns hitbox knockback (death supplies Vector2.ZERO, player.gd:511),
 *     so the knockback helpers below apply to Unit-derived enemies only.
 */

#ifndef BT_PHYSICS_MATH_H
#define BT_PHYSICS_MATH_H

/* ------------------------------------------------------------------ */
/* Cross-target plumbing.  brotato.h (the shared core) may predefine these  */
/* before including this header; the defaults below compile unchanged in   */
/* plain C, C++, CUDA and Metal and pull in nothing but sqrt.              */
/* ------------------------------------------------------------------ */

/* Address-space qualifier.  Empty outside CUDA/HIP toolchains (this includes
 * Metal, whose shader functions need no host/device annotation). */
#ifndef BT_HD
#if defined(__CUDACC__) || defined(__HIPCC__)
#define BT_HD __host__ __device__
#else
#define BT_HD
#endif
#endif

/* Pointer address space.  MSL requires an explicit one on every pointer
 * (including function parameters, where it does NOT default); it is empty on
 * every other target.  The shared core's brotato_rng.h defines the same three
 * names, so this fallback only applies when this header is used standalone. */
#ifndef BT_THREAD
#if defined(__METAL_VERSION__)
#define BT_THREAD thread
#else
#define BT_THREAD
#endif
#endif

/* sqrtf for float math.  Metal gets metal::sqrt from metal_stdlib; everything
 * else gets sqrtf from <math.h>.  Predefine BT_SQRTF to override.
 *
 * MSL gets `metal::precise::sqrt`: the default metal::sqrt is NOT correctly
 * rounded (measured 740/4096 sampled inputs differ from the host sqrtf by
 * 1 ulp, even under MTLMathModeSafe) while precise::sqrt matched the host on
 * all 4096. The core is one numerical definition across CPU/CUDA/Metal, so the
 * device has to use the correctly rounded variant. */
#ifndef BT_SQRTF
#if defined(__METAL_VERSION__)
#include <metal_stdlib>
#define BT_SQRTF(x) metal::precise::sqrt(x)
#else
#include <math.h>
#define BT_SQRTF(x) sqrtf(x)
#endif
#endif

/* Minimal 2D vector.  Predefine BT_VEC2_DEFINED in the core to reuse the
 * core's own struct (it must then provide the same by-value layout). */
#ifndef BT_VEC2_DEFINED
#define BT_VEC2_DEFINED
typedef struct BtVec2 {
    float x;
    float y;
} BtVec2;
#endif

/* knockback / pull constants, unit.gd:19-20 and the literal weights in
 * unit.gd:234-239. */
#define BT_KNOCKBACK_HITBOX_WEIGHT 0.2f  /* unit.gd:281  (0.2 * hitbox direction) */
#define BT_KNOCKBACK_ATTACKER_WEIGHT 0.8f /* unit.gd:281  (0.8 * attacker->victim)  */
#define BT_KNOCKBACK_DECAY_WEIGHT 0.1f   /* unit.gd:234  linear_interpolate(Vector2.ZERO, 0.1) */
#define BT_KNOCKBACK_FULL_SCALE 100.0f   /* unit.gd:239  (100 - resistance * 100) */
#define BT_PULL_SPEED_DIVISOR 6.0f        /* unit.gd:411-412,427-430 */
#define BT_PULL_BUFFER_CAP 150.0f         /* unit.gd:416 min(distance / 3.0, 150.0) */
#define BT_PULL_DIST_STEP 3.0f            /* unit.gd:416 distance / 3.0 */
#define BT_PULL_DMG_THRESHOLD 200.0f      /* unit.gd:19  THRESHOLD_DIST_TO_LOWER_DMG_ON_PULL */
#define BT_MIN_DEATH_KNOCKBACK 15.0f      /* unit.gd:20  MIN_DEATH_KNOCKBACK_AMOUNT */

/* ------------------------------------------------------------------ */
/* scalar helpers                                                      */
/* ------------------------------------------------------------------ */

/* GameScript max(a, b) / min(a, b) / abs(a) for floats. */
BT_HD static inline float bt_minf(float a, float b) { return a < b ? a : b; }
BT_HD static inline float bt_maxf(float a, float b) { return a > b ? a : b; }
BT_HD static inline float bt_absf(float a) { return a < 0.0f ? -a : a; }

/* GDScript/Godot clamp(value, min, max) on one component; Vector2.clamp is a
 * non-clamping suggestion but Unit uses the scalar clamp from the global
 * namespace, so the semantics are Math::clamp with the arguments in
 * (value, min, max) order. */
BT_HD static inline float bt_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* math_funcs: float -> int assignment truncates toward zero (GDScript used to
 * convert a float argument for a typed `int` parameter, e.g. add_decaying_speed). */
BT_HD static inline int bt_float_to_int(float v) { return (int)v; }

/* ------------------------------------------------------------------ */
/* BtVec2 primitives (Vector2 semantics)                               */
/* ------------------------------------------------------------------ */

BT_HD static inline BtVec2 bt_v2(float x, float y) {
    BtVec2 v;
    v.x = x;
    v.y = y;
    return v;
}

BT_HD static inline BtVec2 bt_v2zero(void) { return bt_v2(0.0f, 0.0f); }

BT_HD static inline BtVec2 bt_v2add(BtVec2 a, BtVec2 b) { return bt_v2(a.x + b.x, a.y + b.y); }
BT_HD static inline BtVec2 bt_v2sub(BtVec2 a, BtVec2 b) { return bt_v2(a.x - b.x, a.y - b.y); }
BT_HD static inline BtVec2 bt_v2mul(BtVec2 a, float s) { return bt_v2(a.x * s, a.y * s); }
/* Vector2::operator/ is a per-component divide, not a reciprocal multiply. */
BT_HD static inline BtVec2 bt_v2div(BtVec2 a, float s) { return bt_v2(a.x / s, a.y / s); }

BT_HD static inline float bt_v2dot(BtVec2 a, BtVec2 b) { return a.x * b.x + a.y * b.y; }
BT_HD static inline float bt_v2lensq(BtVec2 a) { return a.x * a.x + a.y * a.y; }
BT_HD static inline float bt_v2len(BtVec2 a) { return BT_SQRTF(a.x * a.x + a.y * a.y); }

/* Vector2::operator== is exact component equality in Godot 3.x. */
BT_HD static inline int bt_v2eq(BtVec2 a, BtVec2 b) { return (a.x == b.x && a.y == b.y) ? 1 : 0; }

/* Vector2::distance_to. */
BT_HD static inline float bt_v2dist(BtVec2 a, BtVec2 b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    return BT_SQRTF(dx * dx + dy * dy);
}

/* Vector2::normalize/normalized: a zero vector stays (0, 0); no NaN. */
BT_HD static inline BtVec2 bt_v2normalized(BtVec2 v) {
    float l = v.x * v.x + v.y * v.y;
    if (l != 0.0f) {
        l = BT_SQRTF(l);
        v.x /= l;
        v.y /= l;
    }
    return v;
}

/* Vector2::direction_to: normalized(to - from). */
BT_HD static inline BtVec2 bt_v2dirto(BtVec2 from, BtVec2 to) {
    return bt_v2normalized(bt_v2sub(to, from));
}

/* Vector2::linear_interpolate(to, weight) == a + weight * (b - a). */
BT_HD static inline BtVec2 bt_v2lerp(BtVec2 a, BtVec2 b, float weight) {
    BtVec2 r = a;
    r.x += weight * (b.x - a.x);
    r.y += weight * (b.y - a.y);
    return r;
}

/* Vector2::project(b) == b * (a . b / |b|^2).  Undefined (NaN) for b == 0,
 * exactly as the engine; Unit guards that case before calling it. */
BT_HD static inline BtVec2 bt_v2project(BtVec2 a, BtVec2 b) {
    return bt_v2mul(b, bt_v2dot(a, b) / bt_v2lensq(b));
}

/* ------------------------------------------------------------------ */
/* Rect2 (position + size) point containment                           */
/* ------------------------------------------------------------------ */

/* Rect2::has_point: minimum edge inclusive, maximum edge exclusive. */
BT_HD static inline int bt_rect_has_point(float rx, float ry, float rw, float rh, float px, float py) {
    if (px < rx) return 0;
    if (py < ry) return 0;
    if (px >= rx + rw) return 0;
    if (py >= ry + rh) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* collision filtering (collision_object_2d_sw.h)                      */
/* ------------------------------------------------------------------ */

/* Layer/mask test: (A.layer & B.mask) || (B.layer & A.mask).  Either object can
 * "see" the other; it is not a mutual-mask agreement. */
BT_HD static inline int bt_filter_pair(unsigned int a_layer, unsigned int a_mask,
                                      unsigned int b_layer, unsigned int b_mask) {
    return ((a_layer & b_mask) != 0u) || ((b_layer & a_mask) != 0u) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* circle / circle overlap with composed offsets                       */
/* ------------------------------------------------------------------ */

/* Discrete Area2D overlap for two circles: the SAT circle/circle kernel used by
 * AreaPair2DSW is a single-axis separation test, so the shapes overlap iff
 * center distance <= rA + rB.  TOUCHING (distance == rA + rB) COUNTS as an
 * overlap.  Not swept; evaluated at the current transforms only. */
BT_HD static inline int bt_circle_overlap(BtVec2 a_center, float a_radius,
                                          BtVec2 b_center, float b_radius) {
    return bt_v2dist(a_center, b_center) <= (a_radius + b_radius) ? 1 : 0;
}

/* Same test with the shape offsets composed onto each root, mirroring
 * "Hurtbox position + Hitbox/Collision position" (see brotato_geometry.h). */
BT_HD static inline int bt_circle_overlap_offset(BtVec2 a_root, BtVec2 a_offset, float a_radius,
                                                 BtVec2 b_root, BtVec2 b_offset, float b_radius) {
    return bt_circle_overlap(bt_v2add(a_root, a_offset), a_radius,
                             bt_v2add(b_root, b_offset), b_radius);
}

/* ------------------------------------------------------------------ */
/* movement normalization and speed                                    */
/* ------------------------------------------------------------------ */

/* get_move_speed: max(0, speed + bonus_speed + decaying_bonus_speed). */
BT_HD static inline float bt_move_speed(float speed, float bonus_speed, float decaying_bonus_speed) {
    return bt_maxf(0.0f, speed + bonus_speed + decaying_bonus_speed);
}

/* get_move_input: movement.normalized() * get_move_speed().  A zero movement
 * vector yields zero (the speed is not applied to a directionless input). */
BT_HD static inline BtVec2 bt_move_input(BtVec2 movement, float speed, float bonus_speed,
                                         float decaying_bonus_speed) {
    return bt_v2mul(bt_v2normalized(movement),
                    bt_move_speed(speed, bonus_speed, decaying_bonus_speed));
}

/* ------------------------------------------------------------------ */
/* knockback direction, replacement, decay                             */
/* ------------------------------------------------------------------ */

/* get_knockback_value scale: 100 - resistance * 100. */
BT_HD static inline float bt_knockback_scale(float resistance) {
    return BT_KNOCKBACK_FULL_SCALE - resistance * BT_KNOCKBACK_FULL_SCALE;
}

/* take_damage: when the hit has no valid attacker, the stored direction is the
 * hitbox direction unchanged. */
BT_HD static inline BtVec2 bt_knockback_direction(BtVec2 hitbox_direction) {
    return hitbox_direction;
}

/* take_damage: with a valid attacker the direction is
 *   0.2 * hitbox_direction + 0.8 * attacker.direction_to(victim)
 * and is deliberately NOT renormalised.  Zero separation makes the second term
 * zero (direction_to is zero-safe). */
BT_HD static inline BtVec2 bt_knockback_direction_weighted(BtVec2 hitbox_direction,
                                                           BtVec2 attacker_position,
                                                           BtVec2 victim_position) {
    BtVec2 to_victim = bt_v2dirto(attacker_position, victim_position);
    return bt_v2add(bt_v2mul(hitbox_direction, BT_KNOCKBACK_HITBOX_WEIGHT),
                    bt_v2mul(to_victim, BT_KNOCKBACK_ATTACKER_WEIGHT));
}

/* take_damage: knockback_vector = direction * amount.  Each hit REPLACES the
 * stored vector; nothing is accumulated. */
BT_HD static inline BtVec2 bt_stored_knockback(BtVec2 direction, float amount) {
    return bt_v2mul(direction, amount);
}

/* get_next_knockback_value: one step of stored-vector decay toward zero.  The
 * weight is fixed per callback; it is not delta-scaled. */
BT_HD static inline BtVec2 bt_knockback_decay(BtVec2 stored) {
    return bt_v2lerp(stored, bt_v2zero(), BT_KNOCKBACK_DECAY_WEIGHT);
}

/* get_knockback_value: the decayed stored vector, scaled by resistance. */
BT_HD static inline BtVec2 bt_knockback_velocity(BtVec2 stored, float resistance) {
    return bt_v2mul(stored, bt_knockback_scale(resistance));
}

/* Result of one knockback tick: the new stored vector plus the velocity it
 * contributes this callback.  Decay happens BEFORE the resistance scaling. */
typedef struct BtKnockbackTick {
    BtVec2 stored;   /* value to write back to knockback_vector */
    BtVec2 velocity; /* velocity component for this callback    */
} BtKnockbackTick;

/* get_next_knockback_value + get_knockback_value in one pure step.  A stored
 * vector with zero length returns zero and leaves `stored` untouched. */
BT_HD static inline BtKnockbackTick bt_knockback_advance(BtVec2 stored, float resistance) {
    BtKnockbackTick out;
    if (bt_v2lensq(stored) == 0.0f) {
        out.stored = stored;
        out.velocity = bt_v2zero();
        return out;
    }
    out.stored = bt_knockback_decay(stored);
    out.velocity = bt_knockback_velocity(out.stored, resistance);
    return out;
}

/* get_next_velocity: move input + this callback's knockback velocity.  The
 * movement vector must already be zeroed if the unit may not move.
 *
 * out_stored_knockback must carry an explicit MSL address space: its only
 * caller (bt_core_step) passes the address of a kernel-local BtVec2, so the
 * parameter is BT_THREAD (empty on host/CUDA, `thread` in MSL; an unqualified
 * pointer parameter is a hard error in MSL, it does NOT default). */
BT_HD static inline BtVec2 bt_next_velocity(BtVec2 movement, float speed, float bonus_speed,
                                            float decaying_bonus_speed, float resistance,
                                            BtVec2 stored_knockback, BT_THREAD BtVec2 *out_stored_knockback) {
    BtKnockbackTick tick = bt_knockback_advance(stored_knockback, resistance);
    if (out_stored_knockback != 0) {
        *out_stored_knockback = tick.stored;
    }
    return bt_v2add(bt_move_input(movement, speed, bonus_speed, decaying_bonus_speed), tick.velocity);
}

/* unit.gd:348: death uses the weighted direction times the RAW hitbox amount,
 * floored at MIN_DEATH_KNOCKBACK_AMOUNT (not the pull/piercing-adjusted amount). */
BT_HD static inline BtVec2 bt_death_knockback(BtVec2 direction, float knockback_amount) {
    return bt_v2mul(direction, bt_maxf(knockback_amount, BT_MIN_DEATH_KNOCKBACK));
}

/* ------------------------------------------------------------------ */
/* piercing knockback correction                                       */
/* ------------------------------------------------------------------ */

/* get_increased_piercing_knockback_amount (unit.gd:445-448).  Resistance 1 is a
 * special case that returns the amount untouched (it would otherwise divide by
 * zero). */
BT_HD static inline float bt_piercing_knockback_amount(float amount, float piercing, float resistance) {
    if (resistance == 1.0f) {
        return amount;
    }
    return amount + (amount * bt_minf(resistance, piercing) / (1.0f - resistance));
}

/* ------------------------------------------------------------------ */
/* negative knockback (pull): amount + decaying speed                  */
/* ------------------------------------------------------------------ */

/* get_knockback_amount_based_on_distance_to_attacker (unit.gd:402-440).  Called
 * only when the incoming base amount is negative.  Radii are the RAW hurtbox
 * CircleShape2D radii (unscaled), movements are the units' _current_movement
 * vectors, and speed is current_stats.speed.  Returns the adjusted amount; the
 * caller must additionally feed -returned_value to bt_add_decaying_speed.
 *
 * The Unit base set_hitbox_damage_modifier() invoked when max_travel exceeds
 * BT_PULL_DMG_THRESHOLD is an empty `pass` and is NOT part of this result (the
 * enemy.gd override is host-side hitbox bookkeeping, see the file header). */
BT_HD static inline float bt_pull_knockback_amount(float base_amount,
                                                   BtVec2 victim_position, float victim_hurt_radius,
                                                   float victim_speed, BtVec2 victim_movement,
                                                   BtVec2 attacker_position, float attacker_hurt_radius,
                                                   float attacker_speed, BtVec2 attacker_movement,
                                                   float piercing, float resistance) {
    BtVec2 direction_to_attacker = bt_v2dirto(victim_position, attacker_position);
    float distance_to_attacker = bt_v2dist(victim_position, attacker_position);
    float movement_dot_product = bt_v2dot(direction_to_attacker, bt_v2normalized(victim_movement));
    float attacker_movement_dot_product = bt_v2dot(direction_to_attacker, bt_v2normalized(attacker_movement));

    float size_factor = victim_hurt_radius;
    float attacker_size_factor = attacker_hurt_radius;
    float speed_factor = movement_dot_product * victim_speed / BT_PULL_SPEED_DIVISOR;
    float attacker_speed_factor = -(attacker_movement_dot_product * attacker_speed / BT_PULL_SPEED_DIVISOR);

    if (attacker_movement_dot_product > 0.0f) {
        attacker_speed_factor *= 0.1f;
    }

    float buffer = size_factor + attacker_size_factor + speed_factor + attacker_speed_factor +
                   bt_minf(distance_to_attacker / BT_PULL_DIST_STEP, BT_PULL_BUFFER_CAP);
    float max_travel_distance = bt_maxf(0.0f, distance_to_attacker - buffer);

    float adjusted_amount = base_amount;
    float expected_travel_distance = bt_absf(base_amount * BT_KNOCKBACK_FULL_SCALE / BT_PULL_SPEED_DIVISOR);
    if (expected_travel_distance > max_travel_distance) {
        adjusted_amount = -(max_travel_distance * BT_PULL_SPEED_DIVISOR / BT_KNOCKBACK_FULL_SCALE);
    }
    if (piercing > 0.0f) {
        adjusted_amount = bt_piercing_knockback_amount(adjusted_amount, piercing, resistance);
    }
    return adjusted_amount;
}

/* add_decaying_speed (unit.gd:730-741).  `value` is an int because GDScript
 * converts the float argument at the typed-parameter boundary (use
 * bt_add_decaying_speed_amt to get that truncation).  Returns the new
 * decaying_bonus_speed; the `emit_signal` side effect is signalling only.
 *
 *   - above -0.7 * speed: no change at all
 *   - above -0.45 * speed: value /= 20 (integer division, truncation to zero)
 *   - clamped so it can never push decaying_bonus_speed below -0.9 * speed */
BT_HD static inline float bt_add_decaying_speed(int value, float current_speed,
                                                float decaying_bonus_speed) {
    if (decaying_bonus_speed < -(current_speed * 0.7f)) {
        return decaying_bonus_speed;
    }
    if (decaying_bonus_speed < -(current_speed * 0.45f)) {
        value /= 20;
    }
    float limit = -current_speed * 0.9f;
    float clamped = ((float)value > limit) ? (float)value : limit;
    return decaying_bonus_speed + (float)bt_float_to_int(clamped);
}

/* add_decaying_speed called with the float pull amount: the float is truncated
 * toward zero before entering the integer body. */
BT_HD static inline float bt_add_decaying_speed_amt(float value, float current_speed,
                                                    float decaying_bonus_speed) {
    return bt_add_decaying_speed(bt_float_to_int(value), current_speed, decaying_bonus_speed);
}

/* ------------------------------------------------------------------ */
/* _integrate_forces: origin clamp, then target projection/snap        */
/* ------------------------------------------------------------------ */

/* unit.gd:177-184.  The PREDICTED origin p + v * h is clamped into the zone
 * rectangle [zone_pos, zone_pos + zone_size] (Rect2::has_point is min-inclusive
 * and max-exclusive, but the clamp bounds are the inclusive end corners), and
 * the velocity is rebuilt as (clamped_next - origin) / h.  The origin itself is
 * never moved here; no radius or shape offset is subtracted. */
BT_HD static inline BtVec2 bt_zone_clamp_velocity(BtVec2 origin, BtVec2 velocity, float step,
                                                  float zone_x, float zone_y, float zone_w,
                                                  float zone_h) {
    BtVec2 next_position = bt_v2add(origin, bt_v2mul(velocity, step));
    if (bt_rect_has_point(zone_x, zone_y, zone_w, zone_h, next_position.x, next_position.y)) {
        return velocity;
    }
    next_position.x = bt_clampf(next_position.x, zone_x, zone_x + zone_w);
    next_position.y = bt_clampf(next_position.y, zone_y, zone_y + zone_h);
    return bt_v2div(bt_v2sub(next_position, origin), step);
}

/* Outcome of one _integrate_forces callback. */
typedef struct BtIntegrateResult {
    BtVec2 origin;   /* state.transform.origin to keep (== input unless snapped) */
    BtVec2 velocity; /* state.linear_velocity to assign */
    int snapped;     /* 1 when the body was teleported onto the target */
} BtIntegrateResult;

/* unit.gd:164-212 in full, in the game's order:
 *   1. the zone clamp above rewrites the cached velocity;
 *   2. if there is no target, or the (post-clamp) velocity is exactly zero, or
 *      the body may not move, or movement is locked, the velocity is assigned
 *      as-is;
 *   3. else, when target distance-squared exceeds step-travel squared, assign
 *      the velocity unchanged;
 *   4. else project the origin->target vector onto the origin->next vector and
 *      snap to the target (velocity ZERO) when
 *      (origin + travel - projected) . (origin - projected) <= 0.
 * The player (MODE_KINEMATIC) returns before this code (unit.gd:169-171), so
 * the clamp and snap apply to non-kinematic (enemy) bodies only. */
BT_HD static inline BtIntegrateResult bt_integrate_forces(BtVec2 origin, BtVec2 velocity, float step,
                                                          float zone_x, float zone_y, float zone_w,
                                                          float zone_h, int has_target, BtVec2 target,
                                                          int can_move, int move_locked) {
    BtIntegrateResult out;
    out.origin = origin;
    out.snapped = 0;

    velocity = bt_zone_clamp_velocity(origin, velocity, step, zone_x, zone_y, zone_w, zone_h);

    if (!has_target || bt_v2eq(velocity, bt_v2zero()) || !can_move || move_locked) {
        out.velocity = velocity;
        return out;
    }

    BtVec2 origin_to_next_origin = bt_v2mul(velocity, step);
    BtVec2 origin_to_target = bt_v2sub(target, origin);

    if (bt_v2lensq(origin_to_target) > bt_v2lensq(origin_to_next_origin)) {
        out.velocity = velocity;
        return out;
    }

    BtVec2 projected_target = bt_v2add(origin, bt_v2project(origin_to_target, origin_to_next_origin));
    BtVec2 next_origin = bt_v2add(origin, origin_to_next_origin);
    if (bt_v2dot(bt_v2sub(next_origin, projected_target), bt_v2sub(origin, projected_target)) <= 0.0f) {
        out.velocity = bt_v2zero();
        out.origin = target;
        out.snapped = 1;
        return out;
    }

    out.velocity = velocity;
    return out;
}

#endif /* BT_PHYSICS_MATH_H */
