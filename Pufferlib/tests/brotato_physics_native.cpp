// Native side of the real-engine differential fixture. No game/recovered scripts.
#include "../ocean/brotato/brotato_physics.h"
static BtPhysWorld world;
static BtPhysicsBody player;
extern "C" {
void bt_fixture_reset() { bt_phys_world_init(&world, 1.0f/60.0f, 8); }
void bt_fixture_body(int index, float x, float y, float radius, float mass, int kinematic) {
    BtPhysicsBody *b = kinematic ? &player : &world.bodies[index];
    *b = bt_phys_body_circle(bt_v2(x,y), bt_v2zero(), radius, mass,
        kinematic ? 2 : 4, kinematic ? 130 : 132,
        kinematic ? BT_PHYS_MODE_KINEMATIC : BT_PHYS_MODE_CHARACTER);
    if (!kinematic && world.body_count <= index) world.body_count = index+1;
}
void bt_fixture_wall(int index) {
    world.bodies[index] = bt_phys_body_rect(bt_v2(-128,512),128,768,128,0);
    if (world.body_count <= index) world.body_count = index+1;
}
void bt_fixture_velocity(int index, float x, float y) { world.bodies[index].vel = bt_v2(x,y); }
void bt_fixture_step() { bt_phys_step(&world); }
void bt_fixture_player_step(float x, float y) {
    const BtVec2 old = player.pos;
    BtVec2 velocity = bt_v2(x,y);
    BtPhysMotionResult result;
    int collided = bt_phys_test_body_motion(&world, &player, bt_v2mul(velocity,1.0f/60.0f),0.08f,&result);
    player.pos = bt_v2add(player.pos,result.motion);
    if (collided) {
        velocity = bt_v2sub(velocity,bt_v2mul(result.collision_normal,bt_v2dot(velocity,result.collision_normal)));
        bt_phys_test_body_motion(&world,&player,bt_v2mul(velocity,1.0f/60.0f),0.08f,&result);
        player.pos = bt_v2add(player.pos,result.motion);
    }
    player.vel = bt_v2mul(bt_v2sub(player.pos,old),60.0f);
}
void bt_fixture_get(int index, int kinematic, float *values) {
    const BtPhysicsBody *b = kinematic ? &player : &world.bodies[index];
    values[0]=b->pos.x; values[1]=b->pos.y; values[2]=b->vel.x; values[3]=b->vel.y;
}
}
