#include "ps2ur/phys_bake.h"

#include "ps2ur/phys_bvh.h"

namespace ps2ur {
namespace phys {

namespace {

// Bounds-checked little-endian reads over the section payload. Every offset
// is validated before use: a .p2b is untrusted input (plan 10's reader
// obligations), and a collision section that indexes past its own payload
// would otherwise be a silent out-of-bounds read into whatever the loader
// put next in the buffer.
struct View {
    const uint8_t* data;
    uint32_t size;

    bool ok(uint32_t offset, uint32_t bytes) const
    {
        // Overflow-safe: offset + bytes could wrap on a hostile file.
        return offset <= size && bytes <= size - offset;
    }
    uint32_t u32(uint32_t at) const
    {
        return static_cast<uint32_t>(data[at]) |
               (static_cast<uint32_t>(data[at + 1]) << 8) |
               (static_cast<uint32_t>(data[at + 2]) << 16) |
               (static_cast<uint32_t>(data[at + 3]) << 24);
    }
    float f32(uint32_t at) const
    {
        const uint32_t bits = u32(at);
        float out;
        __builtin_memcpy(&out, &bits, 4);
        return out;
    }
    Vec3 vec3(uint32_t at) const
    {
        return Vec3{f32(at), f32(at + 4), f32(at + 8)};
    }
};

constexpr uint32_t kHeaderBytes = 32;
constexpr uint32_t kColliderRecordBytes = 48;

} // namespace

bool load_physics(const io::P2bFile& file, StaticMesh* out_mesh,
                  BakeInfo* out_info, const char** out_error)
{
    const char* error = nullptr;
    BakeInfo info;
    StaticMesh mesh;

    const io::P2bSection* section = file.find(io::kSectionPhysics);
    if (section == nullptr) {
        // A scene with no collision at all is legal: not every scene needs
        // physics, and refusing to load one would be wrong.
        if (out_mesh != nullptr) {
            *out_mesh = StaticMesh{};
        }
        if (out_info != nullptr) {
            *out_info = info;
        }
        if (out_error != nullptr) {
            *out_error = "";
        }
        return true;
    }

    const View v{section->data, section->size};
    if (!v.ok(0, kHeaderBytes)) {
        error = "PHYS header truncated";
    }

    uint32_t collider_count = 0, triangle_count = 0, node_count = 0;
    uint32_t vertex_count = 0, colliders_at = 0, nodes_at = 0;
    uint32_t triangles_at = 0, vertices_at = 0;

    if (error == nullptr) {
        // Header: counts then offsets, all u32. Offsets are relative to the
        // section payload so the section can be relocated freely.
        collider_count = v.u32(0);
        node_count = v.u32(4);
        triangle_count = v.u32(8);
        vertex_count = v.u32(12);
        colliders_at = v.u32(16);
        nodes_at = v.u32(20);
        triangles_at = v.u32(24);
        vertices_at = v.u32(28);

        if (collider_count > kMaxColliders) {
            error = "PHYS has more colliders than the runtime can hold";
        } else if (!v.ok(colliders_at, collider_count * kColliderRecordBytes)) {
            error = "PHYS collider table outside the section";
        } else if (!v.ok(nodes_at, node_count * 32u)) {
            error = "PHYS BVH nodes outside the section";
        } else if (!v.ok(triangles_at, triangle_count * 8u)) {
            error = "PHYS BVH triangles outside the section";
        } else if (!v.ok(vertices_at, vertex_count * 12u)) {
            error = "PHYS vertices outside the section";
        } else if (vertex_count > 65536u) {
            // Triangle indices are u16, so more vertices than that could not
            // be addressed. Catching it here beats a silent wrap.
            error = "PHYS has more than 65536 collision vertices";
        } else if ((reinterpret_cast<uintptr_t>(section->data) & 15u) != 0u) {
            error = "PHYS payload is not 16-byte aligned";
        }
    }

    // The BVH is used in place, so it is validated in place before anything
    // points at it.
    if (error == nullptr && node_count > 0) {
        mesh.nodes = reinterpret_cast<const BvhNode*>(section->data + nodes_at);
        mesh.node_count = node_count;
        mesh.triangles =
            reinterpret_cast<const BvhTriangle*>(section->data + triangles_at);
        mesh.triangle_count = triangle_count;
        mesh.vertices = reinterpret_cast<const Vec3*>(section->data + vertices_at);
        mesh.vertex_count = vertex_count;
        // validate_bvh reports success by writing an EMPTY string, not a
        // null one, so its return value is what decides -- assigning
        // straight into 'error' would make every valid tree look like a
        // failure with no message.
        const char* bvh_error = nullptr;
        if (!validate_bvh(mesh, &bvh_error)) {
            error = bvh_error;
            mesh = StaticMesh{};
        }
    }

    if (error != nullptr) {
        if (out_error != nullptr) {
            *out_error = error;
        }
        return false;
    }

    // Only now, with everything checked, is the module touched. Parsing into
    // locals first is what makes a malformed file leave the running world
    // alone rather than half-replacing it.
    for (uint32_t i = 0; i < collider_count; ++i) {
        const uint32_t at = colliders_at + i * kColliderRecordBytes;
        Collider c;
        const uint32_t kind = v.u32(at + 0);
        c.kind = kind <= 3u ? static_cast<ColliderKind>(kind)
                            : ColliderKind::Sphere;
        const uint32_t flags = v.u32(at + 4);
        c.is_trigger = (flags & 1u) != 0u;
        c.enabled = (flags & 2u) != 0u;
        c.axis = static_cast<CapsuleAxis>((flags >> 2) & 3u);
        c.layer = static_cast<uint8_t>(v.u32(at + 8) & 31u);
        c.entity = static_cast<int32_t>(v.u32(at + 12));
        c.center = v.vec3(at + 16);
        c.half_extents = v.vec3(at + 28);
        c.height = v.f32(at + 40);
        // The body index is a RUNTIME concern: the exporter writes -1 and
        // whoever creates the Rigidbody wires it up. A file that claimed a
        // body index would be describing state it cannot know.
        c.body = -1;
        if (add_collider(c) < 0) {
            error = "collider table full while loading PHYS";
            break;
        }
    }

    info.collider_count = collider_count;
    info.node_count = node_count;
    info.triangle_count = triangle_count;
    info.vertex_count = vertex_count;

    if (out_mesh != nullptr) {
        *out_mesh = mesh;
    }
    if (out_info != nullptr) {
        *out_info = info;
    }
    if (out_error != nullptr) {
        *out_error = error == nullptr ? "" : error;
    }
    return error == nullptr;
}

} // namespace phys
} // namespace ps2ur
