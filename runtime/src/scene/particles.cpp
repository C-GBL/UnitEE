// PS2ParticleSystem simulation (M12.5 task 4, ADR-011).
//
// Fixed pools, no allocation, semi-implicit Euler, linear ramps evaluated at
// draw time from ttl/life. Expiry compacts by swap-with-last, so the pool is
// always dense and the renderer never tests liveness.
//
// Randomness is xorshift32 seeded per system: cheap integer math (safe on
// the EE), and a system replays identically after a reload -- which is what
// lets a golden image pin a particle burst at all.
#include "ps2ur/p2b_scene.h"

namespace ps2ur {
namespace scene {

namespace {

inline float frand(uint32_t& state) // [0, 1)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
}

inline float frand_signed(uint32_t& state) // [-1, 1)
{
    return frand(state) * 2.0f - 1.0f;
}

// Uniform direction; rejection-sampled like the shim's Random.onUnitSphere.
inline Vec3 random_direction(uint32_t& state)
{
    for (;;) {
        const Vec3 v{frand_signed(state), frand_signed(state),
                     frand_signed(state)};
        const float sq = v.x * v.x + v.y * v.y + v.z * v.z;
        if (sq > 1e-6f && sq <= 1.0f) {
            const float inv = 1.0f / __builtin_sqrtf(sq);
            return Vec3{v.x * inv, v.y * inv, v.z * inv};
        }
    }
}

} // namespace

int32_t World::particle_system_for_entity(int32_t entity_index) const
{
    for (uint32_t i = 0; i < m_particle_count; ++i) {
        if (m_particle_emitters[i].entity == entity_index) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

void World::particle_play(uint32_t system)
{
    if (system >= m_particle_count) {
        return;
    }
    ParticleSystemState& state = m_particle_states[system];
    state.playing = true;
    state.spawn_accumulator = 0.0f;
    const uint32_t burst = m_particle_emitters[system].burst_count;
    if (burst > 0) {
        particle_emit(system, burst);
    }
}

void World::particle_stop(uint32_t system)
{
    if (system < m_particle_count) {
        // Unity's Stop(): emission ceases, live particles play out.
        m_particle_states[system].playing = false;
    }
}

void World::particle_emit(uint32_t system, uint32_t count)
{
    if (system >= m_particle_count) {
        return;
    }
    const ParticleEmitter& emitter = m_particle_emitters[system];
    ParticleSystemState& state = m_particle_states[system];
    const uint32_t cap =
        emitter.max_particles < kMaxParticlesPerSystem ? emitter.max_particles
                                                       : kMaxParticlesPerSystem;

    // The entity's world matrix, for world-space spawning. Local-space
    // systems spawn in emitter space and the renderer applies the matrix.
    const bool world_space = (emitter.flags & 8u) != 0u;
    static const Mat4 kIdentity = mat4_identity();
    const Mat4& w = emitter.entity >= 0
                        ? world_matrix(static_cast<uint32_t>(emitter.entity))
                        : kIdentity;

    for (uint32_t n = 0; n < count; ++n) {
        if (state.count >= cap) {
            return; // the budget is the contract (ADR-011)
        }
        Vec3 pos{0, 0, 0};
        Vec3 dir{0, 0, 1};
        switch (emitter.shape) {
            case 0: { // sphere: surface direction, interior position
                dir = random_direction(state.rng);
                const float r = emitter.shape_a * frand(state.rng);
                pos = Vec3{dir.x * r, dir.y * r, dir.z * r};
                break;
            }
            case 1: { // cone along +Z: disc base, direction within the angle
                const float angle_rad =
                    emitter.shape_a * (3.14159265f / 180.0f);
                const float t = frand(state.rng) * 6.2831853f;
                const float rr = emitter.shape_b * frand(state.rng);
                pos = Vec3{__builtin_cosf(t) * rr, __builtin_sinf(t) * rr, 0};
                const float spread = __builtin_sinf(angle_rad);
                dir = Vec3{__builtin_cosf(t) * spread * frand(state.rng),
                           __builtin_sinf(t) * spread * frand(state.rng),
                           1.0f};
                const float len = __builtin_sqrtf(
                    dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
                dir = Vec3{dir.x / len, dir.y / len, dir.z / len};
                break;
            }
            default: { // box: interior position, +Z direction
                pos = Vec3{emitter.shape_a * frand_signed(state.rng),
                           emitter.shape_b * frand_signed(state.rng),
                           emitter.shape_c * frand_signed(state.rng)};
                break;
            }
        }
        Vec3 vel{dir.x * emitter.speed, dir.y * emitter.speed,
                 dir.z * emitter.speed};
        if (world_space) {
            // Rotate+translate position, rotate velocity, by the entity's
            // world matrix (column-major, columns are basis vectors).
            const Vec3 p = pos;
            pos = Vec3{w.m[0] * p.x + w.m[4] * p.y + w.m[8] * p.z + w.m[12],
                       w.m[1] * p.x + w.m[5] * p.y + w.m[9] * p.z + w.m[13],
                       w.m[2] * p.x + w.m[6] * p.y + w.m[10] * p.z + w.m[14]};
            const Vec3 v = vel;
            vel = Vec3{w.m[0] * v.x + w.m[4] * v.y + w.m[8] * v.z,
                       w.m[1] * v.x + w.m[5] * v.y + w.m[9] * v.z,
                       w.m[2] * v.x + w.m[6] * v.y + w.m[10] * v.z};
        }
        Particle& p = state.particles[state.count++];
        p.pos = pos;
        p.vel = vel;
        p.ttl = emitter.lifetime > 0.01f ? emitter.lifetime : 0.01f;
        p.life = p.ttl;
    }
}

void World::update_particles(float dt)
{
    for (uint32_t s = 0; s < m_particle_count; ++s) {
        const ParticleEmitter& emitter = m_particle_emitters[s];
        ParticleSystemState& state = m_particle_states[s];

        if (state.pending_emit > 0) {
            const uint32_t burst = state.pending_emit;
            state.pending_emit = 0;
            particle_emit(s, burst);
        }

        // Rate emission while playing.
        if (state.playing && emitter.emission_rate > 0.0f) {
            state.spawn_accumulator += emitter.emission_rate * dt;
            uint32_t to_spawn = 0;
            while (state.spawn_accumulator >= 1.0f) {
                state.spawn_accumulator -= 1.0f;
                ++to_spawn;
            }
            if (to_spawn > 0) {
                particle_emit(s, to_spawn);
            }
        }

        // Integration + expiry, dense pool, swap-with-last.
        const float g = emitter.gravity * 9.81f * dt;
        for (uint32_t i = 0; i < state.count;) {
            Particle& p = state.particles[i];
            p.life -= dt;
            if (p.life <= 0.0f) {
                state.particles[i] = state.particles[--state.count];
                continue;
            }
            p.vel.y -= g; // world -Y, or local -Y by definition (ADR-011)
            p.pos.x += p.vel.x * dt;
            p.pos.y += p.vel.y * dt;
            p.pos.z += p.vel.z * dt;
            ++i;
        }
    }
}

} // namespace scene
} // namespace ps2ur
