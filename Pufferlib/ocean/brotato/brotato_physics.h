/* brotato_physics.h -- the Brotato Bull/Danger-5 body contact solver.
 *
 * This is a directly transcribed port of the GodotPhysics2D body-contact path
 * used by the recovered game.  Every constant, ordering rule and equation is
 * taken from Godot Engine at the pinned commit
 *
 *     0a9011743f8b62e5f4505fd17ca61fc3621394ef   (tag 3.7-dev1)
 *
 * from these files (all MIT, all original Godot code -- the 2D solver is the
 * in-tree GodotPhysics backend, not Box2D and not a third-party solver):
 *
 *   servers/physics_2d/body_pair_2d_sw.{h,cpp}      BodyPair2DSW contact cache
 *   servers/physics_2d/body_2d_sw.{h,cpp}           Body2DSW state + integration
 *   servers/physics_2d/collision_solver_2d_sat.cpp  SAT narrowphase kernels
 *   servers/physics_2d/step_2d_sw.cpp               pipeline ordering
 *   servers/physics_2d/space_2d_sw.cpp              defaults + test_body_motion
 *   servers/physics_2d/shape_2d_sw.{h,cpp}          circle/rectangle supports
 *   servers/physics_2d/collision_object_2d_sw.h     layer/mask filter
 *   core/math/vector2.{h,cpp}                       Vector2 semantics
 *
 * Godot Engine is MIT-licensed; the transcribed arithmetic is redistributed
 * under the following notice:
 *
 *   Copyright (c) 2014-present Godot Engine contributors
 *   Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to
 *   permit persons to whom the Software is furnished to do so, subject to
 *   the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included
 *   in all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 *   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 *   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 *   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS MODELLED (and why it is not an approximation)
 * ---------------------------------------------------------------------------
 * The Brotato scene graph fixes every input the general engine solver would
 * otherwise vary:
 *
 *   - Enemy bodies are RigidBody2D MODE_CHARACTER (server BODY_MODE_CHARACTER
 *     = 3): Body2DSW::set_mode gives _inv_inertia = 0 and angular_velocity = 0
 *     (body_2d_sw.cpp:238-244).  Rotational response is therefore *identically*
 *     zero, not dropped: every `r x j` / inv_inertia term in the general
 *     equation is multiplied by zero.
 *   - PhysicsMaterial friction is 0 on every entity and bounce is the engine
 *     default 0 (physics_material.cpp:82-86).  The fields are kept and the
 *     full combine equations are transcribed; combine_friction = |min(fA,fB)|
 *     is 0 against a wall too, because one operand is always 0.
 *   - gravity / linear damp / angular damp are 0 with custom_integrator=true
 *     (project.godot:2895-2902; physics_body_2d.cpp:667-673), so
 *     Body2DSW::integrate_forces performs no force integration and the game
 *     scripts own linear_velocity through the force-integration callback.
 *   - The player is MODE_KINEMATIC (server BODY_MODE_KINEMATIC = 1).  Its
 *     body-vs-wall contacts are discarded by BodyPair2DSW::setup's
 *     "both modes <= KINEMATIC" branch (body_pair_2d_sw.cpp:200-210), so the
 *     player never receives solver impulses; walls are resolved only by
 *     Space2DSW::test_body_motion, which is transcribed at the bottom.
 *
 * ---------------------------------------------------------------------------
 * KNOWN FIDELITY BOUNDARIES (measured, not hand-waved)
 * ---------------------------------------------------------------------------
 *   1. Broadphase surrogate.  Godot uses BroadPhase2DHashGrid (use_bvh=false).
 *      A BodyPair2DSW exists iff the two collision-object AABBs intersect and
 *      the layer/mask filter passes and not both are STATIC
 *      (broad_phase_2d_hash_grid.cpp:37-100, space_2d_sw.cpp:1233-1260).  This
 *      port reproduces exactly that predicate over a fixed body array.  The
 *      hash-grid cell quantisation only affects which elements are *visited*;
 *      the final acceptance test is `aabb.intersects`, so the live pair set
 *      matches.  Pair *iteration order* is by ascending (i,j) instead of the
 *      engine's hash-bucket order, which can change the order in which
 *      equal-depth contacts are visited across frames (the solver is a
 *      Gauss-Seidel iteration and is order-sensitive at that level).  Bitwise
 *      cross-build equality is therefore NOT claimed.
 *   2. Anguland.  inv_inertia is stored and used in the mass precompute so the
 *      equation is literal, but is 0 for every body this game creates.
 *   3. Recursion.  Godot solves per island with 8 iterations over the island's
 *      constraint list.  This port keeps one flat pair table and applies 8
 *      iterations over the live pairs; the island set of an enemy crowd is the
 *      whole crowd, so the iteration structure is the same.  A pair table
 *      spanning disjoint crowds solves them in the same 8 passes rather than
 *      in independent passes -- identical results for a single connected
 *      component, which is what the game has.
 *   4. Bodies never sleep (CHARACTER: sleep_test returns !active,
 *      body_2d_sw.cpp:611-628).
 *   5. Shape set is CIRCLE and RECTANGLE only.  Those are the only two shapes
 *      any physical body in the recovered scenes uses (circle bodies, and the
 *      four generated axis-aligned wall rectangles).  An unsupported shape
 *      combination is reported through bt_phys_unsupported(), never silently
 *      mis-solved.
 *
 * ---------------------------------------------------------------------------
 * DESIGN CONSTRAINTS
 * ---------------------------------------------------------------------------
 *   - POD state, zero per-step allocation, no host API, no Env dependency.
 *   - Reuses BtVec2 / bt_v2* / BT_HD / BT_SQRTF from brotato_physics_math.h
 *     (owned by the PhysicsMath slice) so there is exactly one vector type.
 *   - GPU-ready: every function is `static inline BT_HD`, state is passed by
 *     pointer, and the large arrays are address-space-configurable through
 *     BT_AS_WORLD / BT_AS_PAIRS (defined by the CUDA/Metal shared layer).
 */

#ifndef BT_PHYSICS_H
#define BT_PHYSICS_H

#include "brotato_physics_math.h"

/* ------------------------------------------------------------------ */
/* address spaces (portability)                                        */
/*                                                                     */
/* brotato_rng.h (included earlier by the shared core) already defines  */
/*   BT_DEVICE / BT_THREAD / BT_CONST_PTR(T)                            */
/* as `device` / `thread` / `constant T*` for Metal Shading Language and */
/* empty on the host and in CUDA.  This header also compiles standalone  */
/* (the native regression test includes only this file), so the same     */
/* guarded defaults are provided here.                                   */
/*                                                                     */
/* MSL requires an explicit address space on EVERY pointer: parameters,  */
/* local pointer variables and member pointers alike.  Classification:   */
/*                                                                       */
/*   BT_AS_WORLD / BT_AS_PAIRS (== device) -- pointers into the          */
/*     persistent world buffer: BtPhysWorld, BtPhysicsBody (bodies[]),   */
/*     BtPhysPair, BtPhysContact (pairs[].contacts[]) and local pointers */
/*     derived from them.                                                */
/*   BT_THREAD -- pointers to per-call scratch owned by the caller: the  */
/*     player BtPhysicsBody, BtPhysMotionResult, contact-point scratch   */
/*     (BtPhysPointPair, BtVec2 point buffers), the scratch scalars the  */
/*     narrowphase/collector write back (r_collided, amount, best_*),    */
/*     and the separating-axis cache (always a local copy, written back  */
/*     into the device pair, so the helper is never called with a device */
/*     pointer).                                                         */
/*                                                                       */
/* Struct/array members are deliberately NOT qualified: MSL rejects      */
/* address spaces on structure members; the address space comes from the */
/* pointer used to reach the containing object.                          */
/* ------------------------------------------------------------------ */
#ifndef BT_DEVICE
#define BT_DEVICE
#endif
#ifndef BT_THREAD
#define BT_THREAD
#endif
#ifndef BT_CONST_PTR
#define BT_CONST_PTR(T) const T *
#endif
#ifndef BT_AS_WORLD
#define BT_AS_WORLD BT_DEVICE
#endif
#ifndef BT_AS_PAIRS
#define BT_AS_PAIRS BT_DEVICE
#endif

/* ------------------------------------------------------------------ */
/* capacities and engine defaults                                      */
/* ------------------------------------------------------------------ */

/* BT_MAX live enemies + 4 generated arena walls (my_tile_map_limits.gd:12-43)
 * + 128 pet bodies.  The player is NOT one of these bodies: it is a kinematic
 * body whose body contacts are all discarded by the engine, and it is driven
 * through bt_phys_test_body_motion() with a caller-owned BtPhysicsBody.
 *
 * The pet bodies sit in their OWN BtPhysWorld (brotato_pet_motion.h): a pet's
 * collision filter (512/640, 0/128 or 0/0 per scene) never overlaps the enemy
 * layer, so the pet island is disjoint from the enemy island and the two can be
 * stepped independently with identical results (the arena walls are STATIC and
 * absorb no impulse).  Both worlds therefore share this capacity: the enemy
 * world keeps body indices 0..BT_MAX+3, the pet world puts its copies of the
 * four walls at 0..3 and the pet slots at 4..131, with body_count set each step
 * to the walls plus the live pet prefix.  C(BT_PHYS_MAX_BODIES,2) pair slots
 * covers every body pair either world can address, so no contact is ever
 * dropped.  brotato_state.h defines BT_MAX *after* including this header, so the
 * value is the literal BT_MAX + 4 = 196 and state.h re-checks it with an
 * #error (a standalone include of this header keeps the same bound). */
#ifndef BT_PHYS_MAX_BODIES
#define BT_PHYS_MAX_BODIES 196
#endif
#if BT_PHYS_MAX_BODIES > 256
#error "physics body indices must fit the byte-sized pair endpoints"
#endif

/* One pair slot per unordered body index pair.  This is the exact worst case:
 * C(BT_PHYS_MAX_BODIES, 2) slots at the 196-body capacity, so the table
 * can never overflow and no contact is ever silently dropped.  Override
 * BT_PHYS_MAX_BODIES to trade memory; a smaller table is detected and reported
 * by bt_phys_overflow(). */
#define BT_PHYS_PAIR_CAPACITY ((BT_PHYS_MAX_BODIES * (BT_PHYS_MAX_BODIES - 1)) / 2)

/* Live contact-cache pool size.  A BodyPair2DSW (and therefore its cached
 * contacts) exists only while the broadphase keeps it, so the cache is held in
 * a compact pool of BT_PHYS_PAIR_POOL live slots rather than the dense
 * C(BT_PHYS_MAX_BODIES,2) table (which is ~99% permanent zeros).  A pair that
 * stops being live frees its slot, so no impulse is ever inherited.  A live pair
 * with no free slot sets pair_overflow -- an EXPLICIT, loud failure, never a
 * silent dropped contact.  Measured peak over long Demon/kite and Demon/swarm
 * runs at the 100-enemy cap: 180 live pairs (see tests/brotato_state_size.cpp);
 * the live plain-enemy count stays capped at 144 even after the BT_MAX raise, so
 * 1024 still carries >3x headroom. */
#ifndef BT_PHYS_PAIR_POOL
#define BT_PHYS_PAIR_POOL 1536
#endif
#if BT_PHYS_PAIR_POOL > 65534
#error "physics pool slots must fit the unsigned-short active/free lists"
#endif
#define BT_PHYS_PAIR_FREE 0xFFFFu  /* pair_map: no live cache for this pair */

/* Body2DSW::MAX_CONTACTS -- body_pair_2d_sw.h:39. */
#define BT_PHYS_MAX_CONTACTS 2

/* Space2DSW contact parameters, space_2d_sw.cpp:1416-1422.  Brotato's
 * project.godot does not override any of them. */
#define BT_PHYS_CONTACT_RECYCLE_RADIUS 1.0f
#define BT_PHYS_CONTACT_MAX_SEPARATION 1.5f
#define BT_PHYS_CONTACT_MAX_ALLOWED_PENETRATION 0.3f
#define BT_PHYS_CONSTRAINT_BIAS 0.2f /* space->constraint_bias (unused by pairs) */

/* Hardcoded contact bias in BodyPair2DSW::setup, body_pair_2d_sw.cpp:337.
 * Only overridden by Shape2DSW::custom_solver_bias, which no Brotato shape
 * sets. */
#define BT_PHYS_CONTACT_BIAS 0.3f

/* physics_2d_server_sw.cpp:1210-1213. */
#define BT_PHYS_SOLVER_ITERATIONS 8

/* test_body_motion constants, space_2d_sw.cpp:36-39. */
#define BT_PHYS_TEST_MOTION_MARGIN_MIN_VALUE 0.0001f
#define BT_PHYS_TEST_MOTION_MIN_CONTACT_DEPTH_FACTOR 0.05f

/* core/math/math_defs.h:34. */
#define BT_PHYS_CMP_EPSILON 0.00001f

/* shape_2d_sw.h:35 -- rectangle support axis validity. */
#define BT_PHYS_SEGMENT_VALID_SUPPORT_THRESHOLD 0.99998f

/* Physics2DServer::BodyMode (physics_2d_server.h:347-352).  NOTE this is the
 * *server* enum, deliberately different from RigidBody2D::Mode. */
enum {
    BT_PHYS_MODE_STATIC = 0,
    BT_PHYS_MODE_KINEMATIC = 1,
    BT_PHYS_MODE_RIGID = 2,
    BT_PHYS_MODE_CHARACTER = 3
};

/* Physics2DServer::ShapeType subset (physics_2d_server.h:218-228) -- the real
 * engine numeric values, because the SAT dispatch orders by them. */
enum {
    BT_PHYS_SHAPE_CIRCLE = 3,
    BT_PHYS_SHAPE_RECTANGLE = 4
};

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

/* One physical body.  Position/velocity are the engine's Body2DSW::transform
 * origin and linear_velocity; `bias_vel` is biased_linear_velocity, which the
 * pair solver accumulates and integrate_velocities adds back in.  `prev_vel`
 * is prev_linear_velocity (used only by the bounce term). */
typedef struct BtPhysicsBody {
    BtVec2 pos;                 /* root position (transform origin) */
    BtVec2 vel;                 /* cached linear velocity (game script -> body) */
    BtVec2 bias_vel;            /* biased_linear_velocity (contact bias channel) */
    BtVec2 prev_vel;            /* prev_linear_velocity, set at integrate_forces */
    BtVec2 offset;              /* circle shape centre, root-local */
    float radius;               /* circle radius */
    float hx, hy;               /* rectangle half-extents (walls) */
    float mass;                 /* Body2DSW mass (engine default 1) */
    float inv_mass;             /* mass > 0 ? 1/mass : 0 */
    float friction;             /* PhysicsMaterial friction (0 in Brotato) */
    float bounce;               /* PhysicsMaterial bounce (engine default 0) */
    float inv_inertia;          /* 0 for STATIC/KINEMATIC/CHARACTER */
    unsigned int layer;         /* collision_layer */
    unsigned int mask;          /* collision_mask */
    unsigned char shape;        /* BT_PHYS_SHAPE_* */
    unsigned char mode;         /* BT_PHYS_MODE_* (server enum) */
    unsigned char active;       /* death disables the shape -> excluded */
    unsigned char pad;
} BtPhysicsBody;

/* BodyPair2DSW::Contact, body_pair_2d_sw.h:55-70. The engine's cached `depth`
 * is write-only in this port; `ra`/`rb` are setup-local because angular impulse
 * is identically absent (see bt_phys_apply_impulse). Keep every float that the
 * next setup or a solve pass actually reads, without changing its arithmetic. */
typedef struct BtPhysContact {
    float acc_normal_impulse;   /* Pn  */
    float acc_tangent_impulse;  /* Pt  */
    float acc_bias_impulse;     /* Pnb */
    float mass_normal;
    float mass_tangent;
    float bias;
    float bounce;
    BtVec2 local_a;             /* contact point in A body-local coords */
    BtVec2 local_b;             /* contact point in B body-local coords */
    BtVec2 normal;              /* unit normal, A -> B */
    unsigned char active;       /* participating this step */
    unsigned char reused;       /* re-detected this step (cache lifetime) */
} BtPhysContact;

/* BodyPair2DSW, body_pair_2d_sw.h:37-92.  The pair object persists for exactly
 * as long as the broadphase keeps it alive, which is what makes the contact
 * cache and its warm-started impulses survive across frames. */
typedef struct BtPhysPair {
    BtVec2 sep_axis;            /* cached separating axis (early-out) */
    BtPhysContact contacts[BT_PHYS_MAX_CONTACTS];
    unsigned char a, b;          /* source body indices 0..195, a < b */
    unsigned char contact_count; /* 0..BT_PHYS_MAX_CONTACTS */
    unsigned char collided;     /* narrowphase found overlap in setup() */
} BtPhysPair;

/* Space2DSW + Step2DSW relevant subset.
 *
 * `active_pairs` is the broadphase output actually consumed by setup/solve:
 * the ascending pair indices live this step, so the solver iterates the live
 * set instead of the whole table, preserving the (i,j) iteration order and
 * every numerical detail.  A pair is retired (cache dropped) as soon as it is
 * not live, which covers inactive/dead bodies, a shrunken body_count, and a
 * recycled body index -- a retired pair re-enters with an empty cache, so no
 * impulse from a previous occupant of the slot can leak in. */
typedef struct BtPhysWorld {
    BtPhysicsBody bodies[BT_PHYS_MAX_BODIES];
    int body_count;
    /* Compact contact-cache pool (see BT_PHYS_PAIR_POOL):
     *   pair_map[pair_index]   pool slot of a live pair, or BT_PHYS_PAIR_FREE
     *   pool_free[pool_free_n-1] LIFO of unused slots (deterministic order)
     *   pairs[slot]            the live record; pairs[BT_PHYS_PAIR_POOL] is a
     *                          shared always-empty record returned by
     *                          bt_phys_pair() for a pair with no live cache
     *   active_pairs[k]        live pool slots, ascending pair-index order */
    BtPhysPair pairs[BT_PHYS_PAIR_POOL + 1];
    unsigned short pair_map[BT_PHYS_PAIR_CAPACITY];
    unsigned short pool_free[BT_PHYS_PAIR_POOL];
    unsigned short pool_free_n;
    unsigned short active_pairs[BT_PHYS_PAIR_POOL]; /* pool slot < 65535 */
    int active_pair_count;
    float step;                 /* p_delta (1/60) */
    int iterations;             /* solver iterations (8) */
    float contact_recycle_radius;
    float contact_max_separation;
    float contact_max_allowed_penetration;
    float constraint_bias;
    float contact_bias;
    unsigned char overflow;     /* body count exceeded BT_PHYS_MAX_BODIES */
    unsigned char pair_overflow;/* a live pair found no free cache slot (loud) */
    unsigned char unsupported;  /* a pair used an unsupported shape combo */
    unsigned char pad0;
} BtPhysWorld;

/* Physics2DServer::MotionResult subset (physics_2d_server.h:466-484). */
typedef struct BtPhysMotionResult {
    BtVec2 motion;
    BtVec2 remainder;
    BtVec2 collision_normal;
    BtVec2 collision_point;
    float collision_depth;
    float collision_safe_fraction;
    float collision_unsafe_fraction;
    float collider_velocity_x;
    float collider_velocity_y;
    int collider;               /* body index, or -1 */
    int collider_shape;
    int collision_local_shape;
    int collided;
} BtPhysMotionResult;

/* One narrowphase contact point pair, in caller (A,B) orientation. */
typedef struct BtPhysPointPair {
    BtVec2 a;
    BtVec2 b;
} BtPhysPointPair;

/* ------------------------------------------------------------------ */
/* world setup                                                         */
/* ------------------------------------------------------------------ */

BT_HD static inline void bt_phys_world_init(BT_AS_WORLD BtPhysWorld *w, float step, int iterations) {
    int i;
    w->body_count = 0;
    w->active_pair_count = 0;
    w->step = step;
    w->iterations = iterations;
    w->contact_recycle_radius = BT_PHYS_CONTACT_RECYCLE_RADIUS;
    w->contact_max_separation = BT_PHYS_CONTACT_MAX_SEPARATION;
    w->contact_max_allowed_penetration = BT_PHYS_CONTACT_MAX_ALLOWED_PENETRATION;
    w->constraint_bias = BT_PHYS_CONSTRAINT_BIAS;
    w->contact_bias = BT_PHYS_CONTACT_BIAS;
    w->overflow = 0;
    w->pair_overflow = 0;
    w->unsupported = 0;
    w->pool_free_n = (unsigned short)BT_PHYS_PAIR_POOL;
    for (i = 0; i < BT_PHYS_PAIR_CAPACITY; i++) w->pair_map[i] = (unsigned short)BT_PHYS_PAIR_FREE;
    for (i = 0; i < BT_PHYS_PAIR_POOL; i++) {
        /* free_stack[i] = POOL-1-i, popped from the end, hands out slot 0 first */
        w->pool_free[i] = (unsigned short)(BT_PHYS_PAIR_POOL - 1 - i);
        w->pairs[i].contact_count = 0;
        w->pairs[i].collided = 0;
        w->pairs[i].sep_axis = bt_v2zero();
    }
    /* shared empty record (never allocated): bt_phys_pair() for a dead pair */
    w->pairs[BT_PHYS_PAIR_POOL].contact_count = 0;
    w->pairs[BT_PHYS_PAIR_POOL].collided = 0;
    w->pairs[BT_PHYS_PAIR_POOL].sep_axis = bt_v2zero();
}

/* The body constructors are called with BOTH a device body
 * (state.bodies[slot], inside the device buffer) and a kernel-local thread body
 * (the player).  MSL cannot share one pointer signature across address spaces,
 * and -- verified on the Metal compiler -- it also rejects making the pointer
 * generic through a template parameter: `template <class TB> void f(TB *b)`
 * fails with "pointer type must have explicit address space qualifier" on the
 * parameter itself, before any deduction. The constructors return a fully
 * initialized value without reading previous body storage:
 *
 *     body = bt_phys_body_circle(pos, ...);
 *
 * This avoids both input copies and reads of uninitialized caller storage.
 */
BT_HD static inline BtPhysicsBody bt_phys_body_circle(BtVec2 pos, BtVec2 offset,
                                                      float radius, float mass,
                                                      unsigned int layer, unsigned int mask,
                                                      unsigned char mode) {
    BtPhysicsBody b;
    b.pos = pos;
    b.vel = bt_v2zero();
    b.bias_vel = bt_v2zero();
    b.prev_vel = bt_v2zero();
    b.offset = offset;
    b.radius = radius;
    b.hx = 0.0f;
    b.hy = 0.0f;
    b.mass = mass;
    b.inv_mass = mass > 0.0f ? 1.0f / mass : 0.0f;
    b.friction = 0.0f;
    b.bounce = 0.0f;
    b.inv_inertia = 0.0f;
    b.layer = layer;
    b.mask = mask;
    b.shape = BT_PHYS_SHAPE_CIRCLE;
    b.mode = mode;
    b.active = 1;
    b.pad = 0;
    return b;
}

BT_HD static inline BtPhysicsBody bt_phys_body_rect(BtVec2 center, float hx, float hy,
                                                    unsigned int layer, unsigned int mask) {
    BtPhysicsBody b;
    b.pos = center;
    b.vel = bt_v2zero();
    b.bias_vel = bt_v2zero();
    b.prev_vel = bt_v2zero();
    b.offset = bt_v2zero();
    b.radius = 0.0f;
    b.hx = hx;
    b.hy = hy;
    b.mass = 0.0f;
    b.inv_mass = 0.0f;
    b.friction = 1.0f;         /* StaticBody2D with no material -> default 1.0 */
    b.bounce = 0.0f;
    b.inv_inertia = 0.0f;
    b.layer = layer;
    b.mask = mask;
    b.shape = BT_PHYS_SHAPE_RECTANGLE;
    b.mode = BT_PHYS_MODE_STATIC;
    b.active = 1;
    b.pad = 0;
    return b;
}

/* ------------------------------------------------------------------ */
/* geometry helpers                                                    */
/* ------------------------------------------------------------------ */

/* Rect2, in the engine's (position, size) form. */
typedef struct BtPhysRect {
    float x, y, w, h;
} BtPhysRect;

BT_HD static inline BtPhysRect bt_phys_rect(float x, float y, float w, float h) {
    BtPhysRect r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    return r;
}

/* Rect2::intersects, rect2.h: touches do NOT count (>= / <=). */
BT_HD static inline int bt_phys_rect_intersects(BtPhysRect a, BtPhysRect b) {
    if (a.x >= b.x + b.w) return 0;
    if (a.x + a.w <= b.x) return 0;
    if (a.y >= b.y + b.h) return 0;
    if (a.y + a.h <= b.y) return 0;
    return 1;
}

BT_HD static inline BtPhysRect bt_phys_rect_grow(BtPhysRect a, float m) {
    BtPhysRect r;
    r.x = a.x - m;
    r.y = a.y - m;
    r.w = a.w + m * 2.0f;
    r.h = a.h + m * 2.0f;
    return r;
}

BT_HD static inline BtPhysRect bt_phys_rect_merge(BtPhysRect a, BtPhysRect b) {
    BtPhysRect r;
    float x0 = bt_minf(a.x, b.x), y0 = bt_minf(a.y, b.y);
    float x1 = bt_maxf(a.x + a.w, b.x + b.w), y1 = bt_maxf(a.y + a.h, b.y + b.h);
    r.x = x0; r.y = y0; r.w = x1 - x0; r.h = y1 - y0;
    return r;
}

/* The body shape's world AABB, matching CollisionObject2DSW::_update_shapes
 * (shape AABB transformed by the body transform * shape transform).
 *
 * Takes the body BY VALUE: this helper is called with both device bodies
 * (bodies[] during pair update) and the caller's thread-local player body
 * (test_motion), and MSL rejects a single pointer qualifier serving both. */
BT_HD static inline BtPhysRect bt_phys_shape_aabb(BtPhysicsBody b, BtVec2 origin) {
    BtVec2 c = bt_v2add(origin, b.offset);
    if (b.shape == BT_PHYS_SHAPE_CIRCLE) {
        return bt_phys_rect(c.x - b.radius, c.y - b.radius,
                            b.radius * 2.0f, b.radius * 2.0f);
    }
    return bt_phys_rect(c.x - b.hx, c.y - b.hy, b.hx * 2.0f, b.hy * 2.0f);
}

/* Vector2::tangent() = (y, -x), vector2.h:159. */
BT_HD static inline BtVec2 bt_phys_tangent(BtVec2 v) { return bt_v2(v.y, -v.x); }

/* Math::is_zero_approx, math_funcs.h:337. */
BT_HD static inline int bt_phys_is_zero_approx(float s) { return bt_absf(s) < BT_PHYS_CMP_EPSILON; }

/* Pair slot index for a < b (row-major upper triangle). */
BT_HD static inline int bt_phys_pair_index(int a, int b) {
    return (a * (2 * BT_PHYS_MAX_BODIES - a - 1)) / 2 + (b - a - 1);
}

/* The live cache record for pair (a,b), or a shared always-empty record when
 * the pair has no live body-pair (retired, or never created). */
BT_HD static inline BT_AS_PAIRS BtPhysPair *bt_phys_pair(BT_AS_WORLD BtPhysWorld *w, int a, int b) {
    int slot;
    if (a > b) { int t = a; a = b; b = t; }
    slot = (int)w->pair_map[bt_phys_pair_index(a, b)];
    if (slot == (int)BT_PHYS_PAIR_FREE) return &w->pairs[BT_PHYS_PAIR_POOL];
    return &w->pairs[slot];
}

/* Release a pool slot and drop its cached contacts (the BodyPair2DSW destroy). */
BT_HD static inline void bt_phys_pool_free(BT_AS_WORLD BtPhysWorld *w, int slot) {
    w->pairs[slot].contact_count = 0;
    w->pairs[slot].collided = 0;
    w->pairs[slot].sep_axis = bt_v2zero();
    w->pool_free[w->pool_free_n++] = (unsigned short)slot;
}

/* Claim a pool slot, or -1 when the pool is exhausted (caller sets the loud
 * pair_overflow).  A claimed slot always starts with an empty cache. */
BT_HD static inline int bt_phys_pool_alloc(BT_AS_WORLD BtPhysWorld *w) {
    if (w->pool_free_n == 0) return -1;
    return (int)w->pool_free[--w->pool_free_n];
}

/* BodyPair2DSW::setup line 200: collided=false when both modes <= KINEMATIC. */
BT_HD static inline int bt_phys_pair_solves(const BT_AS_WORLD BtPhysicsBody *a,
                                            const BT_AS_WORLD BtPhysicsBody *b) {
    if (a->mode <= BT_PHYS_MODE_KINEMATIC && b->mode <= BT_PHYS_MODE_KINEMATIC) return 0;
    return bt_filter_pair(a->layer, a->mask, b->layer, b->mask);
}

/* ------------------------------------------------------------------ */
/* narrowphase: SAT, transcribed from collision_solver_2d_sat.cpp      */
/* ------------------------------------------------------------------ */

/* Shape2DSW::project_range (shape_2d_sw.h:175-186 circle, :163 rect corners).
 * `cast` union-extends the range by `motion` exactly as
 * DEFAULT_PROJECT_RANGE_CAST (shape_2d_sw.h:141-151). */
BT_HD static inline void bt_phys_shape_project(int kind, BtVec2 c, float r, float hx, float hy,
                                               BtVec2 axis, int cast, BtVec2 motion,
                                               BT_THREAD float *out_min, BT_THREAD float *out_max) {
    float mn, mx;
    if (kind == BT_PHYS_SHAPE_CIRCLE) {
        float d = bt_v2dot(axis, c);
        float scale = bt_v2len(axis); /* basis is identity */
        mn = d - r * scale;
        mx = d + r * scale;
    } else {
        int i;
        float rmax = -1e20f, rmin = 1e20f;
        for (i = 0; i < 4; i++) {
            float sx = (float)((i & 1) * 2 - 1) * hx;
            float sy = (float)((i >> 1) * 2 - 1) * hy;
            float d = axis.x * (c.x + sx) + axis.y * (c.y + sy);
            if (d > rmax) rmax = d;
            if (d < rmin) rmin = d;
        }
        mn = rmin;
        mx = rmax;
    }
    if (cast) {
        BtVec2 c2 = bt_v2add(c, motion);
        float mn2, mx2;
        if (kind == BT_PHYS_SHAPE_CIRCLE) {
            float d = bt_v2dot(axis, c2);
            float scale = bt_v2len(axis);
            mn2 = d - r * scale;
            mx2 = d + r * scale;
        } else {
            int i;
            float rmax = -1e20f, rmin = 1e20f;
            for (i = 0; i < 4; i++) {
                float sx = (float)((i & 1) * 2 - 1) * hx;
                float sy = (float)((i >> 1) * 2 - 1) * hy;
                float d = axis.x * (c2.x + sx) + axis.y * (c2.y + sy);
                if (d > rmax) rmax = d;
                if (d < rmin) rmin = d;
            }
            mn2 = rmin;
            mx2 = rmax;
        }
        mn = bt_minf(mn, mn2);
        mx = bt_maxf(mx, mx2);
    }
    *out_min = mn;
    *out_max = mx;
}

typedef struct BtPhysSat {
    float best_depth;
    BtVec2 best_axis;
} BtPhysSat;

/* SeparatorAxisTest2D::test_axis, collision_solver_2d_sat.cpp:243-305.
 * Returns 1 when the axis still contains the origin (penetration possible),
 * 0 when it separates.  On separation *sep_axis is written (line 256-260). */
BT_HD static inline int bt_phys_sat_test_axis(BT_THREAD BtPhysSat *s, BtVec2 axis,
                                              int ka, BtVec2 ca, float ra, float hxa, float hya,
                                              BtVec2 motion_a, int cast_a,
                                              int kb, BtVec2 cb, float rb, float hxb, float hyb,
                                              int with_margin, float ma, float mb,
                                              BT_THREAD BtVec2 *sep_axis) {
    float min_a, max_a, min_b, max_b, dmin, dmax;
    if (bt_phys_is_zero_approx(axis.x) && bt_phys_is_zero_approx(axis.y)) {
        axis = bt_v2(0.0f, 1.0f); /* "strange case, try an upwards separator" */
    }
    bt_phys_shape_project(ka, ca, ra, hxa, hya, axis, cast_a, motion_a, &min_a, &max_a);
    bt_phys_shape_project(kb, cb, rb, hxb, hyb, axis, 0, bt_v2zero(), &min_b, &max_b);
    if (with_margin) {
        min_a -= ma; max_a += ma;
        min_b -= mb; max_b += mb;
    }
    min_b -= (max_a - min_a) * 0.5f;
    max_b += (max_a - min_a) * 0.5f;
    dmin = min_b - (min_a + max_a) * 0.5f;
    dmax = max_b - (min_a + max_a) * 0.5f;
    if (dmin > 0.0f || dmax < 0.0f) {
        if (sep_axis) *sep_axis = axis;
        return 0;
    }
    dmin = bt_absf(dmin);
    if (dmax < dmin) {
        if (dmax < s->best_depth) { s->best_depth = dmax; s->best_axis = axis; }
    } else {
        if (dmin < s->best_depth) { s->best_depth = dmin; s->best_axis = bt_v2mul(axis, -1.0f); }
    }
    return 1;
}

/* RectangleShape2DSW::get_circle_axis, shape_2d_sw.h:338-346 (translation-only
 * rectangle transform).  Axis points from the circle centre toward the chosen
 * rectangle corner. */
BT_HD static inline BtVec2 bt_phys_circle_axis(BtVec2 rect_center, float hx, float hy,
                                               BtVec2 circle_center) {
    BtVec2 lv = bt_v2sub(circle_center, rect_center);
    float sx = lv.x < 0.0f ? -hx : hx;
    float sy = lv.y < 0.0f ? -hy : hy;
    BtVec2 hep = bt_v2(rect_center.x + sx, rect_center.y + sy);
    return bt_v2normalized(bt_v2sub(hep, circle_center));
}

/* RectangleShape2DSW::get_supports, shape_2d_sw.cpp:288-322.  Writes 1 or 2
 * shape-local support points; returns the count. */
BT_HD static inline int bt_phys_rect_supports(BtVec2 n, float hx, float hy, BT_THREAD BtVec2 *out) {
    float he[2];
    float nn[2];
    int i;
    he[0] = hx; he[1] = hy;
    nn[0] = n.x; nn[1] = n.y;
    for (i = 0; i < 2; i++) {
        float dp = nn[i];
        float sgn, s0[2], s1[2];
        if (bt_absf(dp) < BT_PHYS_SEGMENT_VALID_SUPPORT_THRESHOLD) continue;
        sgn = dp > 0.0f ? 1.0f : -1.0f;
        s0[i] = he[i] * sgn; s0[1 - i] = he[1 - i];
        s1[i] = he[i] * sgn; s1[1 - i] = -he[1 - i];
        out[0] = bt_v2(s0[0], s0[1]);
        out[1] = bt_v2(s1[0], s1[1]);
        return 2;
    }
    out[0] = bt_v2(n.x < 0.0f ? -hx : hx, n.y < 0.0f ? -hy : hy);
    return 1;
}

/* Geometry::get_closest_point_to_segment_uncapped_2d, geometry.h:484-495. */
BT_HD static inline BtVec2 bt_phys_closest_on_segment(BtVec2 p, BtVec2 s0, BtVec2 s1) {
    BtVec2 d = bt_v2sub(p, s0);
    BtVec2 n = bt_v2sub(s1, s0);
    float l2 = bt_v2lensq(n);
    if (l2 < 1e-20f) return s0;
    return bt_v2add(s0, bt_v2mul(n, bt_v2dot(n, d) / l2));
}

/* CollisionSolver2DSW::solve -> sat_2d_calculate_penetration, restricted to
 * the CIRCLE x {CIRCLE, RECTANGLE} kernels the game actually uses.
 *
 * Points are world-space.  The engine's shape-frame arithmetic reduces to this
 * because every body transform here is translation-only (character bodies have
 * angular_velocity 0 and the walls are axis-aligned): the SAT axis is a world
 * direction, supports are world points, and the A-local conversion the engine
 * does is a pure translation that cancels in every equation the caller uses.
 *
 * Engine behaviour reproduced exactly:
 *   - type-order dispatch with the collector `swap` flag toggled when the
 *     caller's A type number is greater than B's (collision_solver_2d_sat.cpp:1363-1371);
 *     `swap` inverts the emitted point pair so callers always receive
 *     (point_A, point_B) in their own A/B orientation.
 *   - test_previous_axis, test_castA, then the per-shape axis list.
 *   - `callback->collided = true` before the early return when no point
 *     callback is supplied (collision_solver_2d_sat.cpp:306-315), which is the
 *     `need_points == 0` boolean mode used by test_motion step 2.
 *   - `*sep_axis` set on separation and zeroed after contacts are generated.
 *
 * Returns the number of emitted point pairs (0, 1 or 2), or -1 for an
 * unsupported shape combination.  *r_collided is 1 when the shapes overlap. */
BT_HD static inline int bt_phys_narrowphase(
        int kind_a, BtVec2 ca, float ra, float hxa, float hya,
        int kind_b, BtVec2 cb, float rb, float hxb, float hyb,
        BtVec2 motion_a, int cast_a, int need_points,
        int with_margin, float margin_a, float margin_b,
        BT_THREAD BtVec2 *sep_axis, BT_THREAD int *r_collided, BT_THREAD BtPhysPointPair *out) {
    BtPhysSat s;
    int swap = 0;
    int circle_circle;
    BtVec2 axis;
    BtVec2 sa, sb;
    BtVec2 sup[2];
    int n;
    *r_collided = 0;

    /* sat_2d_calculate_penetration: if (type_A > type_B) swap.  Note the
     * engine swaps the *shapes* twice on this path (CollisionSolver2DSW::solve
     * swaps type/margin, then sat_2d_calculate_penetration swaps shape/margin
     * again), so the net effect is: shapes reordered, margins stay attached to
     * their positional index (margin_a still means "the caller's A margin").
     * Casting is only exercised with the moving circle as the caller's A
     * (test_motion), where no swap happens. */
    if (kind_a > kind_b) {
        int tk = kind_a; kind_a = kind_b; kind_b = tk;
        { BtVec2 t = ca; ca = cb; cb = t; }
        { float t = ra; ra = rb; rb = t; }
        { float t = hxa; hxa = hxb; hxb = t; }
        { float t = hya; hya = hyb; hyb = t; }
        swap = 1;
    }
    if (kind_a != BT_PHYS_SHAPE_CIRCLE) return -1;
    circle_circle = (kind_b == BT_PHYS_SHAPE_CIRCLE);
    if (!circle_circle && kind_b != BT_PHYS_SHAPE_RECTANGLE) return -1;

    s.best_depth = 1e15f;
    s.best_axis = bt_v2zero();

    /* test_previous_axis */
    if (sep_axis && !bt_v2eq(*sep_axis, bt_v2zero())) {
        if (!bt_phys_sat_test_axis(&s, *sep_axis, kind_a, ca, ra, hxa, hya, motion_a, cast_a,
                                   kind_b, cb, rb, hxb, hyb, with_margin, margin_a, margin_b,
                                   sep_axis)) {
            return 0;
        }
    }
    /* test_cast */
    if (cast_a) {
        BtVec2 na = bt_v2normalized(motion_a);
        if (!bt_phys_sat_test_axis(&s, na, kind_a, ca, ra, hxa, hya, motion_a, cast_a,
                                   kind_b, cb, rb, hxb, hyb, with_margin, margin_a, margin_b,
                                   sep_axis)) {
            return 0;
        }
        if (!bt_phys_sat_test_axis(&s, bt_phys_tangent(na), kind_a, ca, ra, hxa, hya, motion_a, cast_a,
                                   kind_b, cb, rb, hxb, hyb, with_margin, margin_a, margin_b,
                                   sep_axis)) {
            return 0;
        }
    }
    /* per-shape axes */
    if (circle_circle) {
        axis = bt_v2normalized(bt_v2sub(ca, cb)); /* TEST_POINT */
        if (!bt_phys_sat_test_axis(&s, axis, kind_a, ca, ra, hxa, hya, motion_a, cast_a,
                                   kind_b, cb, rb, hxb, hyb, with_margin, margin_a, margin_b,
                                   sep_axis)) {
            return 0;
        }
    } else {
        if (!bt_phys_sat_test_axis(&s, bt_v2(1.0f, 0.0f), kind_a, ca, ra, hxa, hya, motion_a, cast_a,
                                   kind_b, cb, rb, hxb, hyb, with_margin, margin_a, margin_b, sep_axis)) {
            return 0;
        }
        if (!bt_phys_sat_test_axis(&s, bt_v2(0.0f, 1.0f), kind_a, ca, ra, hxa, hya, motion_a, cast_a,
                                   kind_b, cb, rb, hxb, hyb, with_margin, margin_a, margin_b, sep_axis)) {
            return 0;
        }
        axis = bt_phys_circle_axis(cb, hxb, hyb, ca);
        if (!bt_phys_sat_test_axis(&s, axis, kind_a, ca, ra, hxa, hya, motion_a, cast_a,
                                   kind_b, cb, rb, hxb, hyb, with_margin, margin_a, margin_b, sep_axis)) {
            return 0;
        }
        if (cast_a) {
            axis = bt_phys_circle_axis(cb, hxb, hyb, bt_v2add(ca, motion_a));
            if (!bt_phys_sat_test_axis(&s, axis, kind_a, ca, ra, hxa, hya, motion_a, cast_a,
                                       kind_b, cb, rb, hxb, hyb, with_margin, margin_a, margin_b, sep_axis)) {
                return 0;
            }
        }
    }

    /* generate_contacts */
    if (s.best_axis.x == 0.0f && s.best_axis.y == 0.0f) return 0;
    *r_collided = 1;
    if (!need_points) return 0; /* callback == null: collide only */

    /* supports_A: circle support along -best_axis */
    sa = bt_v2mul(s.best_axis, -ra);
    sa = bt_v2add(ca, sa);
    if (with_margin) sa = bt_v2add(sa, bt_v2mul(s.best_axis, -margin_a));

    if (circle_circle) {
        sb = bt_v2mul(s.best_axis, rb);
        sb = bt_v2add(cb, sb);
        if (with_margin) sb = bt_v2add(sb, bt_v2mul(s.best_axis, margin_b));
        if (swap) { out[0].a = sb; out[0].b = sa; } else { out[0].a = sa; out[0].b = sb; }
        if (sep_axis && !bt_v2eq(*sep_axis, bt_v2zero())) *sep_axis = bt_v2zero();
        return 1;
    }

    /* supports_B: rectangle */
    n = bt_phys_rect_supports(s.best_axis, hxb, hyb, sup);
    {
        int i;
        for (i = 0; i < n; i++) {
            sup[i] = bt_v2add(cb, sup[i]);
            if (with_margin) sup[i] = bt_v2add(sup[i], bt_v2mul(s.best_axis, margin_b));
        }
    }
    if (n == 1) {
        if (swap) { out[0].a = sup[0]; out[0].b = sa; } else { out[0].a = sa; out[0].b = sup[0]; }
    } else {
        BtVec2 closest = bt_phys_closest_on_segment(sa, sup[0], sup[1]);
        if (swap) { out[0].a = closest; out[0].b = sa; } else { out[0].a = sa; out[0].b = closest; }
    }
    if (sep_axis && !bt_v2eq(*sep_axis, bt_v2zero())) *sep_axis = bt_v2zero();
    return 1;
}

/* ------------------------------------------------------------------ */
/* BodyPair2DSW: contact cache, setup, solve                           */
/* ------------------------------------------------------------------ */

/* Body2DSW::apply_impulse, body_2d_sw.h:214-217.  The angular term
 * (`angular_velocity += _inv_inertia * r.cross(j)`) is omitted because every
 * body mode here sets _inv_inertia = 0 (body_2d_sw.cpp:212-244); BtPhysicsBody
 * carries inv_inertia for the mass precompute so the equation stays literal. */
BT_HD static inline void bt_phys_apply_impulse(BT_AS_WORLD BtPhysicsBody *b, BtVec2 j) {
    b->vel = bt_v2add(b->vel, bt_v2mul(j, b->inv_mass));
}

/* Body2DSW::apply_bias_impulse, body_2d_sw.h:223-226. */
BT_HD static inline void bt_phys_apply_bias_impulse(BT_AS_WORLD BtPhysicsBody *b, BtVec2 j) {
    b->bias_vel = bt_v2add(b->bias_vel, bt_v2mul(j, b->inv_mass));
}

/* BodyPair2DSW::_contact_added_callback, body_pair_2d_sw.cpp:37-93.
 * `pa`/`pb` arrive in the pair's A-local frame (the frame the SAT ran in:
 * A at the origin, B at offset_B): the engine's local_A is
 * `A->get_inv_transform().basis_xform(p_point_A)`, a pure basis xform, so a
 * translation-only A transform leaves the point as-is, and local_B is
 * `basis_xform(p_point_B - offset_B)`, i.e. `pb - offset_B`.  Keeping the
 * contacts in that frame -- rather than re-materializing world points -- is
 * what preserves the engine's exact rounding of `normal`, which is a
 * near-cancelling difference of two ~43 px points. */
BT_HD static inline void bt_phys_pair_add_contact(BT_AS_WORLD BtPhysWorld *w, BT_AS_PAIRS BtPhysPair *p,
                                                 const BT_AS_WORLD BtPhysicsBody *A,
                                                 const BT_AS_WORLD BtPhysicsBody *B,
                                                 BtVec2 offset_b,
                                                 BtVec2 pa, BtVec2 pb) {
    BtVec2 local_a = pa;
    BtVec2 local_b = bt_v2sub(pb, offset_b);
    (void)A; (void)B;
    int new_index = p->contact_count;
    int i;
    float recycle_r2;
    BtPhysContact c;

    if (new_index >= BT_PHYS_MAX_CONTACTS + 1) return; /* engine ERR_FAIL_COND */

    c.acc_normal_impulse = 0.0f;
    c.acc_tangent_impulse = 0.0f;
    c.acc_bias_impulse = 0.0f;
    c.mass_normal = 0.0f;
    c.mass_tangent = 0.0f;
    c.bias = 0.0f;
    c.bounce = 0.0f;
    c.local_a = local_a;
    c.local_b = local_b;
    c.normal = bt_v2normalized(bt_v2sub(pa, pb));
    c.active = 0;
    c.reused = 1;

    /* attempt to determine if the contact will be reused */
    recycle_r2 = w->contact_recycle_radius * w->contact_recycle_radius;
    for (i = 0; i < p->contact_count; i++) {
        BT_AS_PAIRS BtPhysContact *e = &p->contacts[i];
        if (bt_v2lensq(bt_v2sub(e->local_a, local_a)) < recycle_r2 &&
            bt_v2lensq(bt_v2sub(e->local_b, local_b)) < recycle_r2) {
            c.acc_normal_impulse = e->acc_normal_impulse;
            c.acc_tangent_impulse = e->acc_tangent_impulse;
            c.acc_bias_impulse = e->acc_bias_impulse;
            new_index = i;
            break;
        }
    }

    /* figure out if the contact amount must be reduced to fit the new contact.
     * Values are copied (never a mixed device/thread pointer) so this compiles
     * under MSL address spaces. */
    if (new_index == BT_PHYS_MAX_CONTACTS) {
        int least_deep = -1;
        float min_depth = 1e10f;
        for (i = 0; i <= p->contact_count; i++) {
            BtVec2 la, lb, nrm, ga, gb;
            float depth;
            if (i == p->contact_count) {
                la = c.local_a;
                lb = c.local_b;
                nrm = c.normal;
            } else {
                la = p->contacts[i].local_a;
                lb = p->contacts[i].local_b;
                nrm = p->contacts[i].normal;
            }
            ga = la;
            gb = bt_v2add(lb, offset_b);
            depth = bt_v2dot(bt_v2sub(ga, gb), nrm);
            if (depth < min_depth) { min_depth = depth; least_deep = i; }
        }
        if (least_deep < p->contact_count) p->contacts[least_deep] = c;
        return;
    }

    p->contacts[new_index] = c;
    if (new_index == p->contact_count) p->contact_count++;
}

/* BodyPair2DSW::_validate_contacts, body_pair_2d_sw.cpp:95-137. */
BT_HD static inline void bt_phys_validate_contacts(BT_AS_PAIRS BtPhysPair *p,
                                                   const BT_AS_WORLD BtPhysicsBody *A,
                                                   const BT_AS_WORLD BtPhysicsBody *B,
                                                   BtVec2 offset_b,
                                                   float max_separation) {
    float max_separation2 = max_separation * max_separation;
    int i;
    (void)A; (void)B;
    for (i = 0; i < p->contact_count; i++) {
        BT_AS_PAIRS BtPhysContact *c = &p->contacts[i];
        int erase = 0;
        if (!c->reused) {
            erase = 1; /* was left behind in previous frame */
        } else {
            BtVec2 ga, gb, axis;
            float depth;
            c->reused = 0;
            ga = c->local_a;
            gb = bt_v2add(c->local_b, offset_b);
            axis = bt_v2sub(ga, gb);
            depth = bt_v2dot(axis, c->normal);
            gb = bt_v2add(gb, bt_v2mul(c->normal, depth));
            gb = bt_v2sub(gb, ga);
            if (depth < -max_separation || bt_v2lensq(gb) > max_separation2) erase = 1;
        }
        if (erase) {
            if (i + 1 < p->contact_count) {
                BtPhysContact tmp = p->contacts[i];
                p->contacts[i] = p->contacts[p->contact_count - 1];
                p->contacts[p->contact_count - 1] = tmp;
            }
            i--;
            p->contact_count--;
        }
    }
}

BT_HD static inline float bt_phys_combine_bounce(const BT_AS_WORLD BtPhysicsBody *a,
                                                 const BT_AS_WORLD BtPhysicsBody *b) {
    return bt_clampf(a->bounce + b->bounce, 0.0f, 1.0f); /* body_pair_2d_sw.cpp:181 */
}

BT_HD static inline float bt_phys_combine_friction(const BT_AS_WORLD BtPhysicsBody *a,
                                                   const BT_AS_WORLD BtPhysicsBody *b) {
    return bt_absf(bt_minf(a->friction, b->friction)); /* body_pair_2d_sw.cpp:185 */
}

/* BodyPair2DSW::setup, body_pair_2d_sw.cpp:189-410.  Returns the engine's
 * `do_process` (1 when at least one contact is active this step). */
BT_HD static inline int bt_phys_pair_setup(BT_AS_WORLD BtPhysWorld *w, BT_AS_PAIRS BtPhysPair *p) {
    BT_AS_WORLD BtPhysicsBody *A = &w->bodies[p->a];
    BT_AS_WORLD BtPhysicsBody *B = &w->bodies[p->b];
    BtVec2 ca, cb;
    BtVec2 offset_b;
    BtVec2 sep;
    BtPhysPointPair pts[BT_PHYS_MAX_CONTACTS];
    int collided = 0;
    int npts, i;
    int do_process = 0;
    float inv_dt = 1.0f / w->step;
    float max_penetration = w->contact_max_allowed_penetration;
    float bias = w->contact_bias;

    if (!bt_phys_pair_solves(A, B)) {
        p->collided = 0;
        return 0;
    }

    /* BodyPair2DSW::setup works in the A-local frame: `offset_B =
     * B->get_transform().get_origin() - A->get_transform().get_origin()`, then
     * `xform_Au` (A untranslated) and `xform_Bu` shifted by A's origin.  Every
     * later equation -- the SAT, the cached contact points, `depth` -- is then a
     * difference of two ~tens-of-pixels vectors instead of two ~hundreds-of-
     * pixels ones.  That is not cosmetic: `normal` is a near-cancelling
     * difference, so world-frame arithmetic rounds it differently in the last
     * bits, and a contact cluster that stays at its combined radius for tens of
     * frames amplifies that into visible drift (match41 ticks 383-455). */
    offset_b = bt_v2sub(B->pos, A->pos);
    bt_phys_validate_contacts(p, A, B, offset_b, w->contact_max_separation);

    ca = A->offset;
    cb = bt_v2add(offset_b, B->offset);
    /* the separating-axis cache lives in the pair (device memory) but the
     * narrowphase takes a thread-local pointer under MSL, so copy in/out. */
    sep = p->sep_axis;
    npts = bt_phys_narrowphase(A->shape, ca, A->radius, A->hx, A->hy,
                               B->shape, cb, B->radius, B->hx, B->hy,
                               bt_v2zero(), 0, 1, 0, 0.0f, 0.0f,
                               &sep, &collided, pts);
    p->sep_axis = sep;
    if (npts < 0) {
        w->unsupported = 1;
        p->collided = 0;
        return 0;
    }
    p->collided = (unsigned char)collided;
    if (!collided) return 0;

    for (i = 0; i < npts; i++) {
        bt_phys_pair_add_contact(w, p, A, B, offset_b, pts[i].a, pts[i].b);
    }

    for (i = 0; i < p->contact_count; i++) {
        BT_AS_PAIRS BtPhysContact *c = &p->contacts[i];
        BtVec2 ga, gb, ra, rb, tangent, P;
        float depth, rnA, rnB, kn, rtA, rtB, kt;
        c->active = 0;
        /* engine: global_A = xform_Au.xform(local_A) (A-local, origin 0) and
         * global_B = xform_Bu.xform(local_B) = local_B + offset_B. */
        ga = c->local_a;
        gb = bt_v2add(c->local_b, offset_b);
        depth = bt_v2dot(c->normal, bt_v2sub(ga, gb));
        if (depth <= 0.0f || !c->reused) continue;

        /* engine rA = global_A = local_A, rB = global_B - offset_B = local_B */
        ra = c->local_a;
        rb = c->local_b;
        c->active = 1;

        /* normal mass */
        rnA = bt_v2dot(ra, c->normal);
        rnB = bt_v2dot(rb, c->normal);
        kn = A->inv_mass + B->inv_mass;
        kn += A->inv_inertia * (bt_v2lensq(ra) - rnA * rnA);
        kn += B->inv_inertia * (bt_v2lensq(rb) - rnB * rnB);
        c->mass_normal = 1.0f / kn;

        /* tangent mass */
        tangent = bt_phys_tangent(c->normal);
        rtA = bt_v2dot(ra, tangent);
        rtB = bt_v2dot(rb, tangent);
        kt = A->inv_mass + B->inv_mass;
        kt += A->inv_inertia * (bt_v2lensq(ra) - rtA * rtA);
        kt += B->inv_inertia * (bt_v2lensq(rb) - rtB * rtB);
        c->mass_tangent = 1.0f / kt;

        c->bias = -bias * inv_dt * bt_minf(0.0f, -depth + max_penetration);

        /* ACCUMULATE_IMPULSES warm start: apply normal + friction impulse */
        P = bt_v2add(bt_v2mul(c->normal, c->acc_normal_impulse),
                     bt_v2mul(tangent, c->acc_tangent_impulse));
        bt_phys_apply_impulse(A, bt_v2mul(P, -1.0f));
        bt_phys_apply_impulse(B, P);

        c->bounce = bt_phys_combine_bounce(A, B);
        if (c->bounce != 0.0f) {
            BtVec2 dv = bt_v2sub(B->prev_vel, A->prev_vel);
            c->bounce = c->bounce * bt_v2dot(dv, c->normal);
        }
        do_process = 1;
    }
    return do_process;
}

/* BodyPair2DSW::solve, body_pair_2d_sw.cpp:411-475, one Gauss-Seidel pass. */
BT_HD static inline void bt_phys_pair_solve(BT_AS_WORLD BtPhysWorld *w, BT_AS_PAIRS BtPhysPair *p) {
    BT_AS_WORLD BtPhysicsBody *A = &w->bodies[p->a];
    BT_AS_WORLD BtPhysicsBody *B = &w->bodies[p->b];
    int i;
    float friction = bt_phys_combine_friction(A, B);

    if (!p->collided) return;

    for (i = 0; i < p->contact_count; i++) {
        BT_AS_PAIRS BtPhysContact *c = &p->contacts[i];
        BtVec2 dv, dbv, tangent, jb, j;
        float vn, vbn, vt, jbn, jbn_old, jn, jn_old, jt_max, jt, jt_old;

        if (!c->active) continue;

        dv = bt_v2sub(B->vel, A->vel);
        dbv = bt_v2sub(B->bias_vel, A->bias_vel);
        vn = bt_v2dot(dv, c->normal);
        vbn = bt_v2dot(dbv, c->normal);
        tangent = bt_phys_tangent(c->normal);
        vt = bt_v2dot(dv, tangent);

        jbn = (c->bias - vbn) * c->mass_normal;
        jbn_old = c->acc_bias_impulse;
        c->acc_bias_impulse = bt_maxf(jbn_old + jbn, 0.0f);
        jb = bt_v2mul(c->normal, c->acc_bias_impulse - jbn_old);
        bt_phys_apply_bias_impulse(A, bt_v2mul(jb, -1.0f));
        bt_phys_apply_bias_impulse(B, jb);

        jn = -(c->bounce + vn) * c->mass_normal;
        jn_old = c->acc_normal_impulse;
        c->acc_normal_impulse = bt_maxf(jn_old + jn, 0.0f);

        jt_max = friction * c->acc_normal_impulse;
        jt = -vt * c->mass_tangent;
        jt_old = c->acc_tangent_impulse;
        c->acc_tangent_impulse = bt_clampf(jt_old + jt, -jt_max, jt_max);

        j = bt_v2add(bt_v2mul(c->normal, c->acc_normal_impulse - jn_old),
                     bt_v2mul(tangent, c->acc_tangent_impulse - jt_old));
        bt_phys_apply_impulse(A, bt_v2mul(j, -1.0f));
        bt_phys_apply_impulse(B, j);
    }
}

/* ------------------------------------------------------------------ */
/* Step2DSW pipeline                                                   */
/* ------------------------------------------------------------------ */

/* One body of the step head: snapshot prev_linear_velocity and clear the biased
 * velocity channel.  bt_phys_begin_step is the ascending body loop over this, so
 * a parallel schedule running it once per body in any order gives the same
 * bodies[] (and mirrors bt_phys_body_integrate_velocities at the tail). */
BT_HD static inline void bt_phys_body_begin_step(BT_AS_WORLD BtPhysicsBody *b) {
    b->prev_vel = b->vel;
    b->bias_vel = bt_v2zero();
}

/* Body2DSW::integrate_forces head, body_2d_sw.cpp:403,497-503: the engine
 * snapshots prev_linear_velocity and clears the biased velocities every step.
 * (Force/gravity/damping integration is skipped for this game: zero gravity,
 * zero damping, custom_integrator=true.) */
BT_HD static inline void bt_phys_begin_step(BT_AS_WORLD BtPhysWorld *w) {
    int i;
    int n = w->body_count > BT_PHYS_MAX_BODIES ? BT_PHYS_MAX_BODIES : w->body_count;
    for (i = 0; i < n; i++) {
        BT_AS_WORLD BtPhysicsBody *b = &w->bodies[i];
        if (!b->active) continue;
        bt_phys_body_begin_step(b);
    }
}

/* Broadphase surrogate: a pair is live iff both bodies are active, the two
 * shape AABBs intersect, the layer/mask filter passes, and not both bodies are
 * STATIC (broad_phase_2d_hash_grid.cpp:37-100).
 *
 * Lifecycle: the scan rebuilds `active_pairs` with the still-live pairs in
 * ascending pair-index order, and any pair that stops being live drops its
 * cached contacts.  Consequences, all of which mirror
 * BroadPhase2DHashGrid::_unpair_attempt destroying the BodyPair2DSW:
 *   - a body going inactive (death disables its shape) retires every pair it
 *     takes part in;
 *   - a shrunken body_count retires pairs whose index falls outside the range
 *     through the previous list, even though the scan never reaches them;
 *   - a retired pair that becomes live again receives an empty cache, so a
 *     recycled body index can never inherit a ghost impulse from its old owner.
 * The live list and pair_map are authoritative; no duplicate per-pair used flag
 * is necessary.
 *
 * The scan is split into the small shared pieces below -- the clamped extent,
 * the pure predicate, the retire decision and the live claim -- so that a
 * parallel schedule (ocean/brotato/brotato_gpu_physics.cuh) reuses the exact
 * same per-pair semantics instead of re-deriving them, while the serial loop
 * below keeps its own AABB/alias optimisations. */

/* Clamped scan extent, reporting a table too small for body_count.  The serial
 * loop below performs exactly this inline (it reads body_count once, before its
 * per-body broadcast); this is the same clamp, shared so the parallel schedule
 * in ocean/brotato/brotato_gpu_physics.cuh cannot drift from it. */
BT_HD static inline int bt_phys_scan_body_count(BT_AS_WORLD BtPhysWorld *w) {
    int n = w->body_count;
    if (n < 0) n = 0;
    if (n > BT_PHYS_MAX_BODIES) { n = BT_PHYS_MAX_BODIES; w->overflow = 1; }
    return n;
}

/* Broadphase surrogate acceptance test, the predicate of the scan below
 * (broad_phase_2d_hash_grid.cpp:37-100): a pair is live iff both bodies are
 * active, they are not both STATIC, the layer/mask filter passes and the two
 * shape AABBs intersect.  Pure (reads bodies only -- no pair state, no world
 * state), so a parallel schedule may evaluate it in any order and get the same
 * per-pair answer the ascending serial walk gets.
 *
 * `ra` is bt_phys_shape_aabb(w->bodies[ia], ...pos), the caller's cached AABB of
 * the first body (the host scan keeps a per-body table); it is unused when that
 * body is inactive. */
BT_HD static inline int bt_phys_body_pair_live(BT_AS_WORLD BtPhysWorld *w, int ia, int ib,
                                               BtPhysRect ra) {
    BT_AS_WORLD const BtPhysicsBody *a = &w->bodies[ia];
    BT_AS_WORLD const BtPhysicsBody *b = &w->bodies[ib];
    if (!a->active || !b->active) return 0;
    if (a->mode == BT_PHYS_MODE_STATIC && b->mode == BT_PHYS_MODE_STATIC) return 0;
    if (!bt_filter_pair(a->layer, a->mask, b->layer, b->mask)) return 0;
    return bt_phys_rect_intersects(ra, bt_phys_shape_aabb(*b, b->pos));
}

/* Retire one pair the previous update left live, called once per entry of that
 * list in its own order: a pair whose two body indices are still inside the new
 * scan range keeps its slot AND cached contacts preserved (which warm-starts
 * the next solve) and is left to the scan below, which
 * retires it if it lost liveness; a pair that fell outside the range is not
 * visited by the scan at all, so it releases its slot and pair_map entry here.
 * Order-sensitive: the released slots are pushed onto the LIFO free stack. */
BT_HD static inline void bt_phys_pair_retire(BT_AS_WORLD BtPhysWorld *w, int slot, int n) {
    BT_AS_PAIRS BtPhysPair *p = &w->pairs[slot];
    if (p->a >= n || p->b >= n) {
        w->pair_map[bt_phys_pair_index(p->a, p->b)] = (unsigned short)BT_PHYS_PAIR_FREE;
        bt_phys_pool_free(w, slot);
    }
}

/* The scan's reject branch for pair pi: a pair that was live last step and is
 * not live now releases its slot and its pair_map entry (the BodyPair2DSW
 * destroy, so a later re-entry cannot inherit its impulses).  A pair with no
 * live cache is left alone.  Order-sensitive within the ascending scan, because
 * the released slots go onto the LIFO free stack the claims pop. */
BT_HD static inline void bt_phys_pair_release(BT_AS_WORLD BtPhysWorld *w, int pi) {
    int slot = (int)w->pair_map[pi];
    if (slot != (int)BT_PHYS_PAIR_FREE) {
        w->pair_map[pi] = (unsigned short)BT_PHYS_PAIR_FREE;
        bt_phys_pool_free(w, slot);
    }
}

/* Live claim for one in-range pair (pi == bt_phys_pair_index(i, j)): a pair
 * claiming a slot for the first time starts with an empty cache and records the
 * two body indices; a pair that was live last step keeps its slot and contacts.
 * Either way it is marked used and appended to active_pairs, which is what makes
 * that list ascending in pair-index order.  Returns 0 when the pool is
 * exhausted (loud pair_overflow, pair left out of the live set, as the engine
 * would leave a contact unsolved rather than silently mis-place an impulse). */
BT_HD static inline int bt_phys_pool_claim(BT_AS_WORLD BtPhysWorld *w, int i, int j, int pi) {
    int slot = (int)w->pair_map[pi];
    BT_AS_PAIRS BtPhysPair *p;
    if (slot == (int)BT_PHYS_PAIR_FREE) {
        slot = bt_phys_pool_alloc(w);
        if (slot < 0) { w->pair_overflow = 1; return 0; } /* loud */
        w->pair_map[pi] = (unsigned short)slot;
        p = &w->pairs[slot];
        p->a = i;
        p->b = j;
        p->contact_count = 0;
        p->collided = 0;
        p->sep_axis = bt_v2zero();
    } else {
        p = &w->pairs[slot];
    }
    w->active_pairs[w->active_pair_count++] = slot;
    return 1;
}

#if defined(__CUDA_ARCH__) || defined(__METAL_VERSION__)
/* The scan below needs both bodies' shape AABBs for every pair it tests.  On
 * the host each body's AABB is a pure function of that body and its position
 * (neither changes during the scan), so it is computed once per body into the
 * small v_aabb[] table and read back L1-resident -- one AABB per body instead
 * of one per pair.  On a device that table would be per-thread local memory,
 * so the device keeps recomputing it inline.  The predicate value is identical
 * either way: this only changes the schedule, never the live set or an order. */
#define BT_PHYS_BROAD_AABB_CACHE 0
#else
#define BT_PHYS_BROAD_AABB_CACHE 1
#endif

BT_HD static inline void bt_phys_update_pairs(BT_AS_WORLD BtPhysWorld *w) {
    int n = w->body_count;
    int i, j, k;
    int nc = 0;
    int prev_count = w->active_pair_count;
    BtPhysRect v_empty = bt_phys_rect(0.0f, 0.0f, 0.0f, 0.0f);
    /* v_cand[i]: 0 = the pair scan can ignore body i entirely, 1 = body i holds
     * no live shape this step but still carries a cached pair, 2 = body i is
     * active.  v_idx lists the non-zero rows in ascending order. */
    unsigned char v_cand[BT_PHYS_MAX_BODIES];
    unsigned short v_idx[BT_PHYS_MAX_BODIES];
#if BT_PHYS_BROAD_AABB_CACHE
    BtPhysRect v_aabb[BT_PHYS_MAX_BODIES];
#endif

    if (n < 0) n = 0;
    if (n > BT_PHYS_MAX_BODIES) { n = BT_PHYS_MAX_BODIES; w->overflow = 1; }

    /* The scan extent is the SCAN CANDIDATE set, not all n slots.  A pair (i,j)
     * can hold a cached pool slot or become live this step only if both bodies
     * are active now or both held a live pair last step (the previous
     * active_pairs list is the ONLY source of persistent pair_map entries, and
     * every entry was live then, so both its bodies were active then).  For any
     * other pair the scan's two branches are strict no-ops: pair_map[pi] is
     * BT_PHYS_PAIR_FREE (so bt_phys_pair_release changes nothing) and the pair
     * predicate is false (a body that is inactive now can never be live).  The
     * scan below therefore visits exactly the pairs with both bodies in this
     * candidate set, in the same ascending pair-index order as the full
     * n^2 walk, and leaves every pool/map state byte-identical -- it only skips
     * work the full walk would have discarded. */
    for (i = 0; i < n; i++) v_cand[i] = 0;
    for (k = 0; k < prev_count; k++) {
        BT_AS_PAIRS BtPhysPair *p = &w->pairs[w->active_pairs[k]];
        if (p->a < n) v_cand[p->a] = 1;
        if (p->b < n) v_cand[p->b] = 1;
    }
    /* Per-call broadcast of the fields the scan reads for body j: the stores to
     * pairs[]/pair_map[] in the inner loop may alias bodies[], so a body field
     * could not otherwise be kept in a register across iterations, and v_aabb
     * removes the per-pair AABB recomputation. */
    for (i = 0; i < n; i++) {
        if (w->bodies[i].active) v_cand[i] = 2;
        if (v_cand[i]) v_idx[nc++] = (unsigned short)i;
    }

#if BT_PHYS_BROAD_AABB_CACHE
#define BT_PHYS_BROAD_AABB(idx) (v_aabb[idx])
#else
#define BT_PHYS_BROAD_AABB(idx) \
    (v_cand[idx] == 2 ? bt_phys_shape_aabb(w->bodies[idx], w->bodies[idx].pos) : v_empty)
#endif

#if BT_PHYS_BROAD_AABB_CACHE
    for (k = 0; k < nc; k++) {
        i = (int)v_idx[k];
        v_aabb[i] = v_cand[i] == 2 ? bt_phys_shape_aabb(w->bodies[i], w->bodies[i].pos)
                                   : v_empty;
    }
#endif

    /* Retire every pair listed by the previous call.  A pair whose body index
     * falls outside the new scan range is not visited below, so its slot is
     * freed now; an in-range pair is only marked unused and is retired by the
     * scan if it lost liveness or kept (slot and cached contacts preserved) if
     * it is still live.  This mirrors BroadPhase2DHashGrid::_unpair_attempt
     * destroying the BodyPair2DSW the moment it stops being live. */
    for (k = 0; k < prev_count; k++) {
        bt_phys_pair_retire(w, w->active_pairs[k], n);
    }
    w->active_pair_count = 0;

    for (int ci = 0; ci < nc; ci++) {
        i = (int)v_idx[ci];
        BT_AS_WORLD BtPhysicsBody *a = &w->bodies[i];
        unsigned char aact = (unsigned char)(v_cand[i] == 2);
        /* Row-invariant fields in locals: the stores to pairs[]/pair_map[] in
         * the inner loop may alias bodies[i], so these loads could not
         * otherwise be hoisted out of the scan. */
        unsigned char astat = (unsigned char)(a->mode == BT_PHYS_MODE_STATIC);
        unsigned int alay = a->layer;
        unsigned int amsk = a->mask;
        BtPhysRect ra = BT_PHYS_BROAD_AABB(i);
        int rowbase = (i * (2 * BT_PHYS_MAX_BODIES - i - 1)) / 2;
        for (int cj = ci + 1; cj < nc; cj++) {
            j = (int)v_idx[cj];
            BT_AS_WORLD BtPhysicsBody *b = &w->bodies[j];
            int pi = rowbase + (j - i - 1);
            int live = 0;
            if (aact && v_cand[j] == 2 &&
                !(astat && b->mode == BT_PHYS_MODE_STATIC) &&
                bt_filter_pair(alay, amsk, b->layer, b->mask)) {
                live = bt_phys_rect_intersects(ra, BT_PHYS_BROAD_AABB(j));
            }
            if (live) {
                /* claim a slot on first sight (or keep the warm-start cache),
                 * mark used and append in ascending pair-index order */
                bt_phys_pool_claim(w, i, j, pi);
            } else {
                bt_phys_pair_release(w, pi);
            }
        }
    }
#undef BT_PHYS_BROAD_AABB
}

/* Step2DSW::_setup_island (step_2d_sw.cpp:55-76): every live pair is set up
 * once, iterating only the live set in ascending pair-index order. */
BT_HD static inline void bt_phys_setup_pairs(BT_AS_WORLD BtPhysWorld *w) {
    int k;
    for (k = 0; k < w->active_pair_count; k++) {
        bt_phys_pair_setup(w, &w->pairs[w->active_pairs[k]]);
    }
}

/* Step2DSW::_solve_island (step_2d_sw.cpp:78-85): `iterations` passes over the
 * live set only. */
BT_HD static inline void bt_phys_solve_pairs(BT_AS_WORLD BtPhysWorld *w) {
    int it, k;
    for (it = 0; it < w->iterations; it++) {
        for (k = 0; k < w->active_pair_count; k++) {
            BT_AS_PAIRS BtPhysPair *p = &w->pairs[w->active_pairs[k]];
            /* bt_phys_pair_solve's first act is `if (!p->collided) return;`
             * (after a pure friction combine), so a pair whose narrowphase
             * found no overlap is a strict no-op for all `iterations` passes:
             * testing it here skips the per-pass call and the combine without
             * changing the solve order of the pairs that do iterate. */
            if (!p->collided) continue;
            bt_phys_pair_solve(w, p);
        }
    }
}

/* One body of Body2DSW::integrate_velocities, body_2d_sw.cpp:535-567:
 *   p' = p + h * (linear_velocity + biased_linear_velocity)
 * STATIC does not move.  KINEMATIC (the player) is never in this table; it is
 * driven by bt_phys_test_body_motion, and the engine's kinematic branch here
 * only snaps to a caller-supplied transform. */
BT_HD static inline void bt_phys_body_integrate_velocities(BT_AS_WORLD BtPhysicsBody *b,
                                                           float step) {
    BtVec2 total;
    if (b->mode == BT_PHYS_MODE_STATIC || b->mode == BT_PHYS_MODE_KINEMATIC) return;
    total = bt_v2add(b->vel, b->bias_vel);
    b->pos = bt_v2add(b->pos, bt_v2mul(total, step));
}

/* Body2DSW::integrate_velocities, body_2d_sw.cpp:535-567 (see the per-body
 * helper); a parallel schedule may run this body loop in any order. */
BT_HD static inline void bt_phys_integrate_velocities(BT_AS_WORLD BtPhysWorld *w) {
    int i;
    int n = w->body_count > BT_PHYS_MAX_BODIES ? BT_PHYS_MAX_BODIES : w->body_count;
    for (i = 0; i < n; i++) {
        BT_AS_WORLD BtPhysicsBody *b = &w->bodies[i];
        if (!b->active) continue;
        bt_phys_body_integrate_velocities(b, w->step);
    }
}

/* One full Step2DSW::step, step_2d_sw.cpp:133-266.  Call the four phases
 * individually when the caller needs to observe contacts between them (the
 * regression tests do); bt_phys_step is the convenience wrapper. */
BT_HD static inline void bt_phys_step(BT_AS_WORLD BtPhysWorld *w) {
    bt_phys_begin_step(w);
    bt_phys_update_pairs(w);
    bt_phys_setup_pairs(w);
    bt_phys_solve_pairs(w);
    bt_phys_integrate_velocities(w);
}

/* Error surfaces (no silent failure). */
BT_HD static inline int bt_phys_overflow(const BT_AS_WORLD BtPhysWorld *w) {
    return (w->overflow || w->pair_overflow) ? 1 : 0;
}
BT_HD static inline int bt_phys_unsupported(const BT_AS_WORLD BtPhysWorld *w) { return w->unsupported ? 1 : 0; }

/* Memory accounting, for callers sizing an env. */
BT_HD static inline int bt_phys_sizeof_pair(void) { return (int)sizeof(BtPhysPair); }
BT_HD static inline int bt_phys_sizeof_body(void) { return (int)sizeof(BtPhysicsBody); }
BT_HD static inline int bt_phys_pair_capacity(void) { return BT_PHYS_PAIR_CAPACITY; }
BT_HD static inline int bt_phys_pair_pool_capacity(void) { return BT_PHYS_PAIR_POOL; }

/* ------------------------------------------------------------------ */
/* Space2DSW::test_body_motion, space_2d_sw.cpp:723-1230               */
/*                                                                     */
/* The player is RigidBody2D MODE_KINEMATIC and unit.gd:135-155 drives  */
/* it with exactly two test_motion calls (motion, then                */
/* velocity.slide(normal) * delta).  This is the exact routine those    */
/* calls run, restricted to the reachable colliders: the player's mask  */
/* (130) admits only layer 128 (the four static arena walls), and       */
/* infinite_inertia=true skips everything that is not STATIC/KINEMATIC. */
/* The routine is transcribed whole -- three stages, recovery factor    */
/* 0.4, the 8-step bisection with its adaptive 0.25/0.5/0.75            */
/* coefficients, and the rest query.                                    */
/* ------------------------------------------------------------------ */

#define BT_PHYS_TM_MAX_RESULTS 32

BT_HD static inline int bt_phys_tm_body_aabb(const BT_THREAD BtPhysicsBody *body, BtVec2 origin,
                                             float margin, BT_THREAD BtPhysRect *out) {
    BtPhysRect a = bt_phys_shape_aabb(*body, origin);
    *out = bt_phys_rect_grow(a, margin);
    return 1;
}

/* _cull_aabb_for_body (space_2d_sw.cpp:1179-1203) reduced to: AABB overlap and
 * the body's own collision filter; plus the infinite-inertia filter the caller
 * applies at the use site (space_2d_sw.cpp:789-796, 966-973). */
BT_HD static inline int bt_phys_tm_collider(const BT_AS_WORLD BtPhysWorld *w,
                                            const BT_THREAD BtPhysicsBody *body,
                                            int idx, BtPhysRect query) {
    const BT_AS_WORLD BtPhysicsBody *o = &w->bodies[idx];
    if (!o->active) return 0;
    if (o->mode != BT_PHYS_MODE_STATIC && o->mode != BT_PHYS_MODE_KINEMATIC) return 0;
    if (!bt_filter_pair(body->layer, body->mask, o->layer, o->mask)) return 0;
    return bt_phys_rect_intersects(query, bt_phys_shape_aabb(*o, o->pos));
}

/* Physics2DServerSW::_shape_col_cbk (physics_2d_server_sw.cpp:150-195), with
 * valid_dir == 0 because no Brotato shape is a one-way collision shape. */
BT_HD static inline void bt_phys_tm_collect(BT_THREAD BtVec2 *sr, BT_THREAD int *amount,
                                            BtVec2 pa, BtVec2 pb) {
    float rel2 = bt_v2lensq(bt_v2sub(pa, pb));
    if (*amount == BT_PHYS_TM_MAX_RESULTS) {
        float min_depth = 1e20f;
        int min_depth_idx = 0;
        int i;
        for (i = 0; i < *amount; i++) {
            float d = bt_v2lensq(bt_v2sub(sr[i * 2 + 0], sr[i * 2 + 1]));
            if (d < min_depth) { min_depth = d; min_depth_idx = i; }
        }
        if (rel2 < min_depth) return;
        sr[min_depth_idx * 2 + 0] = pa;
        sr[min_depth_idx * 2 + 1] = pb;
    } else {
        sr[*amount * 2 + 0] = pa;
        sr[*amount * 2 + 1] = pb;
        (*amount)++;
    }
}

/* _rest_cbk_result (space_2d_sw.cpp:563-590). */
BT_HD static inline void bt_phys_tm_rest(BtVec2 pa, BtVec2 pb, float min_allowed_depth,
                                         BT_THREAD float *best_len, BT_THREAD BtVec2 *best_contact,
                                         BT_THREAD BtVec2 *best_normal,
                                         int idx, BT_THREAD int *best_obj) {
    BtVec2 rel = bt_v2sub(pb, pa);
    float len = bt_v2len(rel);
    if (len < min_allowed_depth) return;
    if (len <= *best_len) return;
    *best_len = len;
    *best_contact = pb;
    *best_normal = bt_v2div(rel, len);
    *best_obj = idx;
}

BT_HD static inline int bt_phys_test_body_motion(const BT_AS_WORLD BtPhysWorld *w,
                                                 const BT_THREAD BtPhysicsBody *body,
                                                 BtVec2 motion, float margin_in,
                                                 BT_THREAD BtPhysMotionResult *r) {
    float margin = bt_maxf(margin_in, BT_PHYS_TEST_MOTION_MARGIN_MIN_VALUE);
    float min_contact_depth = margin * BT_PHYS_TEST_MOTION_MIN_CONTACT_DEPTH_FACTOR;
    float motion_length = bt_v2len(motion);
    BtVec2 motion_normal = bt_v2div(motion, motion_length);
    BtVec2 body_transform = body->pos;
    BtPhysRect body_aabb;
    BtVec2 sr[BT_PHYS_TM_MAX_RESULTS * 2];
    int recovered = 0;
    float safe = 1.0f, unsafe = 1.0f;
    int best_shape = -1;
    int collided = 0;
    int n = w->body_count > BT_PHYS_MAX_BODIES ? BT_PHYS_MAX_BODIES : w->body_count;
    int idx;

    if (r) {
        r->collider = -1;
        r->collider_shape = 0;
        r->collided = 0;
    }

    bt_phys_tm_body_aabb(body, body_transform, margin, &body_aabb);

    /* STEP 1 -- free body if stuck (space_2d_sw.cpp:786-885). */
    {
        int recover_attempts = 4;
        do {
            int amount = 0;
            int any = 0;
            for (idx = 0; idx < n; idx++) {
                BtPhysPointPair pts[BT_PHYS_MAX_CONTACTS];
                int flag = 0, nc, i;
                if (!bt_phys_tm_collider(w, body, idx, body_aabb)) continue;
                nc = bt_phys_narrowphase(body->shape, bt_v2add(body_transform, body->offset),
                                         body->radius, body->hx, body->hy,
                                         w->bodies[idx].shape, bt_v2add(w->bodies[idx].pos, w->bodies[idx].offset),
                                         w->bodies[idx].radius, w->bodies[idx].hx, w->bodies[idx].hy,
                                         bt_v2zero(), 0, 1, 1, margin, 0.0f, 0, &flag, pts);
                (void)flag;
                if (nc < 0) continue; /* unsupported: cannot happen for circle/rect */
                for (i = 0; i < nc; i++) {
                    bt_phys_tm_collect(sr, &amount, pts[i].a, pts[i].b);
                    any = 1;
                }
            }
            if (!any) break;
            recovered = 1;
            {
                BtVec2 recover_motion = bt_v2zero();
                int i;
                for (i = 0; i < amount; i++) {
                    BtVec2 a = sr[i * 2 + 0];
                    BtVec2 b = sr[i * 2 + 1];
                    BtVec2 nn = bt_v2normalized(bt_v2sub(a, b));
                    float d = bt_v2dot(nn, b);
                    float depth = bt_v2dot(nn, bt_v2add(a, recover_motion)) - d;
                    if (depth > min_contact_depth + BT_PHYS_CMP_EPSILON) {
                        recover_motion = bt_v2sub(recover_motion,
                                                 bt_v2mul(nn, (depth - min_contact_depth) * 0.4f));
                    }
                }
                if (bt_v2eq(recover_motion, bt_v2zero())) break;
                body_transform = bt_v2add(body_transform, recover_motion);
                body_aabb.x += recover_motion.x;
                body_aabb.y += recover_motion.y;
            }
            recover_attempts--;
        } while (recover_attempts);
    }

    /* STEP 2 -- attempt motion (space_2d_sw.cpp:887-1069). */
    {
        BtPhysRect motion_aabb;
        int stuck = 0;
        float best_safe = 1.0f, best_unsafe = 1.0f;
        motion_aabb = bt_phys_rect_merge(body_aabb,
                                         bt_phys_rect(body_aabb.x + motion.x, body_aabb.y + motion.y,
                                                      body_aabb.w, body_aabb.h));
        for (idx = 0; idx < n; idx++) {
            int flag = 0, nc, k;
            float low = 0.0f, hi = 1.0f, fraction_coeff = 0.5f;
            if (!bt_phys_tm_collider(w, body, idx, motion_aabb)) continue;
            /* does it collide if going all the way? */
            nc = bt_phys_narrowphase(body->shape, bt_v2add(body_transform, body->offset),
                                     body->radius, body->hx, body->hy,
                                     w->bodies[idx].shape, bt_v2add(w->bodies[idx].pos, w->bodies[idx].offset),
                                     w->bodies[idx].radius, w->bodies[idx].hx, w->bodies[idx].hy,
                                     motion, 1, 0, 0, 0.0f, 0.0f, 0, &flag, 0);
            if (nc < 0 || !flag) continue;
            /* does it collide right now? */
            nc = bt_phys_narrowphase(body->shape, bt_v2add(body_transform, body->offset),
                                     body->radius, body->hx, body->hy,
                                     w->bodies[idx].shape, bt_v2add(w->bodies[idx].pos, w->bodies[idx].offset),
                                     w->bodies[idx].radius, w->bodies[idx].hx, w->bodies[idx].hy,
                                     bt_v2zero(), 0, 0, 0, 0.0f, 0.0f, 0, &flag, 0);
            if (nc < 0) continue;
            if (flag) { stuck = 1; break; }

            for (k = 0; k < 8; k++) {
                float fraction = low + (hi - low) * fraction_coeff;
                BtVec2 sep = motion_normal;
                nc = bt_phys_narrowphase(body->shape, bt_v2add(body_transform, body->offset),
                                         body->radius, body->hx, body->hy,
                                         w->bodies[idx].shape, bt_v2add(w->bodies[idx].pos, w->bodies[idx].offset),
                                         w->bodies[idx].radius, w->bodies[idx].hx, w->bodies[idx].hy,
                                         bt_v2mul(motion, fraction), 1, 0, 0, 0.0f, 0.0f, &sep, &flag, 0);
                if (nc < 0) continue;
                if (flag) {
                    hi = fraction;
                    fraction_coeff = (k == 0 || low > 0.0f) ? 0.5f : 0.25f;
                } else {
                    low = fraction;
                    fraction_coeff = (k == 0 || hi < 1.0f) ? 0.5f : 0.75f;
                }
            }
            if (low < best_safe) { best_safe = low; best_unsafe = hi; }
        }
        if (stuck) {
            safe = 0.0f;
            unsafe = 0.0f;
            best_shape = 0;
        } else if (best_safe != 1.0f && best_safe < safe) {
            safe = best_safe;
            unsafe = best_unsafe;
            best_shape = 0;
        }
    }

    /* STEP 3 -- rest query (space_2d_sw.cpp:1071-1203). */
    if (recovered || safe < 1.0f) {
        float min_allowed_depth = bt_minf(motion_length, min_contact_depth);
        float best_len = 0.0f;
        BtVec2 best_contact = bt_v2zero();
        BtVec2 best_normal = bt_v2zero();
        int best_obj = -1;
        BtPhysRect aabb3 = body_aabb;
        BtVec2 ugt;
        int from_shape, to_shape, j;
        if (safe >= 1.0f) best_shape = -1;
        ugt = bt_v2add(body_transform, bt_v2mul(motion, unsafe));
        aabb3.x += motion.x * unsafe;
        aabb3.y += motion.y * unsafe;
        from_shape = best_shape != -1 ? best_shape : 0;
        to_shape = best_shape != -1 ? best_shape + 1 : 1;

        for (j = from_shape; j < to_shape; j++) {
            for (idx = 0; idx < n; idx++) {
                BtPhysPointPair pts[BT_PHYS_MAX_CONTACTS];
                int flag = 0, nc, i;
                if (!bt_phys_tm_collider(w, body, idx, aabb3)) continue;
                nc = bt_phys_narrowphase(body->shape, bt_v2add(ugt, body->offset),
                                         body->radius, body->hx, body->hy,
                                         w->bodies[idx].shape, bt_v2add(w->bodies[idx].pos, w->bodies[idx].offset),
                                         w->bodies[idx].radius, w->bodies[idx].hx, w->bodies[idx].hy,
                                         bt_v2zero(), 0, 1, 1, margin, 0.0f, 0, &flag, pts);
                (void)flag;
                if (nc < 0) continue;
                for (i = 0; i < nc; i++) {
                    bt_phys_tm_rest(pts[i].a, pts[i].b, min_allowed_depth,
                                    &best_len, &best_contact, &best_normal, idx, &best_obj);
                }
            }
        }

        if (best_len != 0.0f) {
            if (r) {
                r->collision_normal = best_normal;
                r->collision_point = best_contact;
                r->collision_depth = best_len;
                r->collision_safe_fraction = safe;
                r->collision_unsafe_fraction = unsafe;
                r->collider = best_obj;
                r->collider_shape = 0;
                r->collision_local_shape = best_shape != -1 ? best_shape : 0;
                r->collider_velocity_x = w->bodies[best_obj].vel.x;
                r->collider_velocity_y = w->bodies[best_obj].vel.y;
                r->motion = bt_v2mul(motion, safe);
                r->remainder = bt_v2sub(motion, r->motion);
                r->motion = bt_v2add(r->motion, bt_v2sub(body_transform, body->pos));
            }
            collided = 1;
        }
    }

    if (!collided && r) {
        r->motion = motion;
        r->remainder = bt_v2zero();
        r->motion = bt_v2add(r->motion, bt_v2sub(body_transform, body->pos));
    }
    if (r) r->collided = collided;
    return collided;
}


#endif /* BT_PHYSICS_H */
