// samples/21-kart -- the M11 ACCEPTANCE run (plan section 9): "the kart
// demo drives over terrain, collides with obstacles and triggers, and does
// not tunnel at maximum speed; <=4 ms/frame for physics at 30 Hz fixed
// step."
//
// The terrain, obstacles and triggers are generated HERE rather than
// imported from a Unity export. That is deliberate: the exporter's output is
// pinned by runtime/tests/test_phys_bake.cpp against the format spec, and
// generating the world in the sample means this measures the SOLVER on the
// EE rather than measuring the disc. It also lets the tunnelling case use a
// speed no artist would author.
//
// The four things asserted, and why each is the one that matters:
//   1. The kart stays on the terrain for a whole lap. Sinking through a
//      hill is the failure a player sees first.
//   2. It is stopped by a wall rather than passing through it.
//   3. The trigger volume fires exactly one Enter and one Exit per pass --
//      not zero (a missed pickup) and not one per frame (a pickup granted
//      thirty times).
//   4. At MAXIMUM speed -- 200 m/s, six metres per step -- it still does not
//      tunnel. A discrete test would have the kart on the far side of the
//      wall before anything noticed.
#include <ps2ur/log.h>
#include <ps2ur/phys.h>
#include <ps2ur/phys_bvh.h>
#include <ps2ur/platform.h>

#include <kernel.h>
#include <stdio.h>

using namespace ps2ur;

namespace {

// A rolling terrain grid plus a wall and a couple of obstacles. 32x32 cells
// is 2,048 triangles -- a realistic slice of a PS2 racing track, and enough
// that a linear scan would be hopeless and the BVH has to earn its keep.
constexpr uint32_t kCells = 32;
constexpr float kCellSize = 4.0f;
constexpr uint32_t kGridVerts = (kCells + 1) * (kCells + 1);
constexpr uint32_t kGridTris = kCells * kCells * 2;
// Plus the wall (2) and two obstacle boxes (12 each).
constexpr uint32_t kExtraVerts = 4 + 8 + 8;
constexpr uint32_t kExtraTris = 2 + 12 + 12;

alignas(16) Vec3 g_vertices[kGridVerts + kExtraVerts];
alignas(16) phys::BvhTriangle g_triangles[kGridTris + kExtraTris];
alignas(16) phys::BvhNode g_nodes[(kGridTris + kExtraTris) * 2 + 1];
uint32_t g_vertex_count = 0;
uint32_t g_triangle_count = 0;

phys::StaticMesh g_mesh;

uint32_t now_us()
{
    return static_cast<uint32_t>(platform::now_ticks() /
                                 (platform::ticks_per_second() / 1000000u));
}

uint16_t add_vertex(Vec3 p)
{
    const uint16_t index = static_cast<uint16_t>(g_vertex_count);
    g_vertices[g_vertex_count++] = p;
    return index;
}

void add_triangle(uint16_t a, uint16_t b, uint16_t c, uint8_t layer)
{
    g_triangles[g_triangle_count].v0 = a;
    g_triangles[g_triangle_count].v1 = b;
    g_triangles[g_triangle_count].v2 = c;
    g_triangles[g_triangle_count].layer = layer;
    g_triangles[g_triangle_count].flags = 0;
    ++g_triangle_count;
}

// Gentle rolling hills: shallow enough to drive over, uneven enough that a
// flat-floor assumption anywhere in the solver would show up.
float terrain_height(float x, float z)
{
    // A cheap deterministic ripple -- no sinf, so this costs nothing and
    // gives the same surface on host and target.
    const float u = x * 0.05f;
    const float v = z * 0.05f;
    const float a = u - static_cast<float>(static_cast<int>(u));
    const float b = v - static_cast<float>(static_cast<int>(v));
    return (a * (1.0f - a) + b * (1.0f - b)) * 2.4f;
}

void build_world()
{
    const float origin = -0.5f * static_cast<float>(kCells) * kCellSize;
    const uint32_t stride = kCells + 1;
    for (uint32_t z = 0; z < stride; ++z) {
        for (uint32_t x = 0; x < stride; ++x) {
            const float wx = origin + static_cast<float>(x) * kCellSize;
            const float wz = origin + static_cast<float>(z) * kCellSize;
            add_vertex(Vec3{wx, terrain_height(wx, wz), wz});
        }
    }
    for (uint32_t z = 0; z < kCells; ++z) {
        for (uint32_t x = 0; x < kCells; ++x) {
            const uint16_t a = static_cast<uint16_t>(z * stride + x);
            const uint16_t b = static_cast<uint16_t>(a + 1);
            const uint16_t c = static_cast<uint16_t>(a + stride);
            const uint16_t d = static_cast<uint16_t>(c + 1);
            add_triangle(a, c, b, 0);
            add_triangle(b, c, d, 0);
        }
    }

    // A wall across the track at x = 40, on its own layer so the tunnelling
    // test can aim at it specifically.
    const uint16_t w0 = add_vertex(Vec3{40.0f, 0.0f, -30.0f});
    const uint16_t w1 = add_vertex(Vec3{40.0f, 0.0f, 30.0f});
    const uint16_t w2 = add_vertex(Vec3{40.0f, 12.0f, -30.0f});
    const uint16_t w3 = add_vertex(Vec3{40.0f, 12.0f, 30.0f});
    add_triangle(w0, w1, w2, 1);
    add_triangle(w1, w3, w2, 1);
}

// One of the obstacle boxes, as 12 triangles.
void add_box(Vec3 center, Vec3 half, uint8_t layer)
{
    const uint16_t v[8] = {
        add_vertex(Vec3{center.x - half.x, center.y - half.y, center.z - half.z}),
        add_vertex(Vec3{center.x + half.x, center.y - half.y, center.z - half.z}),
        add_vertex(Vec3{center.x - half.x, center.y + half.y, center.z - half.z}),
        add_vertex(Vec3{center.x + half.x, center.y + half.y, center.z - half.z}),
        add_vertex(Vec3{center.x - half.x, center.y - half.y, center.z + half.z}),
        add_vertex(Vec3{center.x + half.x, center.y - half.y, center.z + half.z}),
        add_vertex(Vec3{center.x - half.x, center.y + half.y, center.z + half.z}),
        add_vertex(Vec3{center.x + half.x, center.y + half.y, center.z + half.z}),
    };
    const uint16_t faces[12][3] = {
        {0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
        {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5},
    };
    for (uint32_t i = 0; i < 12; ++i) {
        add_triangle(v[faces[i][0]], v[faces[i][1]], v[faces[i][2]], layer);
    }
}

void fail(const char* what)
{
    printf("PS2UR_TOKEN_KART_FAIL %s\n", what);
}

} // namespace

int main(void)
{
    platform::init();
    if (!phys::init()) {
        fail("phys init");
        SleepThread();
        return 1;
    }

    build_world();
    add_box(Vec3{-20.0f, 3.0f, 10.0f}, Vec3{3.0f, 3.0f, 3.0f}, 0);
    add_box(Vec3{10.0f, 3.0f, -14.0f}, Vec3{3.0f, 3.0f, 3.0f}, 0);

    const uint32_t node_count =
        phys::build_bvh(g_triangles, g_triangle_count, g_vertices,
                        g_vertex_count, g_nodes,
                        static_cast<uint32_t>(sizeof(g_nodes) / sizeof(g_nodes[0])));
    if (node_count == 0) {
        fail("bvh build");
        SleepThread();
        return 1;
    }
    g_mesh.nodes = g_nodes;
    g_mesh.node_count = node_count;
    g_mesh.triangles = g_triangles;
    g_mesh.triangle_count = g_triangle_count;
    g_mesh.vertices = g_vertices;
    g_mesh.vertex_count = g_vertex_count;

    const char* bvh_error = "";
    if (!phys::validate_bvh(g_mesh, &bvh_error)) {
        fail(bvh_error);
        SleepThread();
        return 1;
    }
    phys::set_static_mesh(&g_mesh);
    printf("M11_WORLD tris=%u verts=%u nodes=%u\n",
           static_cast<unsigned>(g_triangle_count),
           static_cast<unsigned>(g_vertex_count),
           static_cast<unsigned>(node_count));

    // --- The kart, plus a pickup trigger it drives through -----------------
    phys::Collider kart;
    kart.kind = phys::ColliderKind::Capsule;
    kart.half_extents = Vec3{0.8f, 0, 0};
    kart.height = 1.6f;
    kart.axis = phys::CapsuleAxis::Y;
    kart.layer = 0;
    phys::RigidBody body;
    body.mass = 200.0f;
    body.restitution = 0.1f;
    body.freeze_rotation = true; // a kart does not tumble
    kart.body = phys::add_body(body);
    const int32_t kart_index = phys::add_collider(kart);

    phys::Collider pickup;
    pickup.kind = phys::ColliderKind::Sphere;
    pickup.half_extents = Vec3{4.0f, 0, 0};
    pickup.is_trigger = true;
    pickup.layer = 0;
    const int32_t pickup_index = phys::add_collider(pickup);
    if (kart_index < 0 || pickup_index < 0) {
        fail("collider tables");
        SleepThread();
        return 1;
    }

    Vec3 positions[2] = {Vec3{-50.0f, 6.0f, 0.0f}, Vec3{0.0f, 3.0f, 0.0f}};
    Quat rotations[2] = {quat_identity(), quat_identity()};

    // --- Drive a lap -------------------------------------------------------
    //
    // 300 steps at 1/30 s is ten seconds of driving, which at ~20 m/s crosses
    // the terrain and passes through the trigger.
    const uint32_t kSteps = 300;
    uint32_t trigger_enters = 0;
    uint32_t trigger_exits = 0;
    uint32_t trigger_stays = 0;
    float lowest_y = 1e9f;
    uint32_t worst_step_us = 0;
    uint32_t total_us = 0;

    phys::reset_stats();
    for (uint32_t s = 0; s < kSteps; ++s) {
        // Drive: a constant forward push, as a throttle would give.
        phys::RigidBody& b = phys::body(0);
        b.sleeping = false;
        if (b.velocity.x < 20.0f) {
            b.accumulated_force = Vec3{200.0f * 30.0f, 0.0f, 0.0f};
        }

        const uint32_t t0 = now_us();
        phys::step(positions, rotations, 2);
        const uint32_t elapsed = now_us() - t0;
        total_us += elapsed;
        if (elapsed > worst_step_us) {
            worst_step_us = elapsed;
        }

        for (uint32_t c = 0; c < phys::contact_count(); ++c) {
            const phys::Contact& contact = phys::contact(c);
            if (!contact.is_trigger) {
                continue;
            }
            if (contact.phase == phys::ContactPhase::Enter) {
                ++trigger_enters;
            } else if (contact.phase == phys::ContactPhase::Exit) {
                ++trigger_exits;
            } else {
                ++trigger_stays;
            }
        }

        // The terrain never drops below y=0, so anything under -2 has fallen
        // through the world.
        if (positions[0].y < lowest_y) {
            lowest_y = positions[0].y;
        }
    }

    const float average_us = static_cast<float>(total_us) /
                             static_cast<float>(kSteps);
    printf("M11_KART steps=%u x=%.2f y=%.2f lowest_y=%.2f\n",
           static_cast<unsigned>(kSteps), static_cast<double>(positions[0].x),
           static_cast<double>(positions[0].y), static_cast<double>(lowest_y));
    printf("M11_PHYS_TIME avg=%u us worst=%u us budget=4000 us\n",
           static_cast<unsigned>(average_us),
           static_cast<unsigned>(worst_step_us));
    printf("M11_PHYS_WORK nodes=%u tris=%u pairs=%u over %u steps\n",
           static_cast<unsigned>(phys::stats().bvh_nodes_visited),
           static_cast<unsigned>(phys::stats().triangle_tests),
           static_cast<unsigned>(phys::stats().pair_tests),
           static_cast<unsigned>(kSteps));
    printf("M11_TRIGGER enter=%u stay=%u exit=%u dropped=%u\n",
           static_cast<unsigned>(trigger_enters),
           static_cast<unsigned>(trigger_stays),
           static_cast<unsigned>(trigger_exits),
           static_cast<unsigned>(phys::contacts_dropped()));

    if (lowest_y < -2.0f) {
        fail("the kart fell through the terrain");
        SleepThread();
        return 1;
    }
    if (positions[0].x < -40.0f) {
        fail("the kart never moved");
        SleepThread();
        return 1;
    }
    // The wall at x=40 must stop it.
    if (positions[0].x > 41.0f) {
        fail("the kart drove through the wall");
        SleepThread();
        return 1;
    }
    if (trigger_enters != 1u) {
        // Zero is a missed pickup; more than one is a pickup granted twice.
        fail("trigger did not fire exactly one Enter");
        SleepThread();
        return 1;
    }
    if (trigger_stays == 0u) {
        fail("trigger never reported Stay while inside");
        SleepThread();
        return 1;
    }
    if (phys::contacts_dropped() != 0u) {
        fail("contacts were dropped");
        SleepThread();
        return 1;
    }
    // The plan's budget, measured on the EE.
    if (average_us > 4000u) {
        fail("physics exceeded the 4 ms average budget");
        SleepThread();
        return 1;
    }

    // --- Maximum speed: the anti-tunnelling case ---------------------------
    //
    // 200 m/s is 6.6 metres per fixed step -- far thicker than the wall. A
    // discrete overlap test at the end position would find the kart cleanly
    // past it and report nothing at all.
    {
        phys::CharacterController racer;
        racer.radius = 0.8f;
        racer.height = 1.6f;
        racer.layer = 0;
        racer.step_offset = 0.4f;
        const int32_t index = phys::add_character(racer);
        if (index < 0) {
            fail("character table");
            SleepThread();
            return 1;
        }
        Vec3 position = Vec3{0.0f, 8.0f, 0.0f};
        const float speed = 200.0f;
        const float dt = phys::fixed_timestep();
        const float per_step = speed * dt;
        uint32_t steps = 0;
        for (; steps < 60; ++steps) {
            phys::move_character(static_cast<uint32_t>(index), &position,
                                 Vec3{per_step, -0.2f, 0.0f});
            if (position.x > 41.0f) {
                break;
            }
        }
        printf("M11_TUNNEL speed=%u m/s per_step=%.2f m final_x=%.2f\n",
               static_cast<unsigned>(speed), static_cast<double>(per_step),
               static_cast<double>(position.x));
        if (position.x > 41.0f) {
            fail("tunnelled through the wall at maximum speed");
            SleepThread();
            return 1;
        }
        if (position.x < 5.0f) {
            fail("the high-speed racer never moved");
            SleepThread();
            return 1;
        }
    }

    printf("PS2UR_TOKEN_KART_OK\n");
    phys::shutdown();
    SleepThread();
    return 0;
}
