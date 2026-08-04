#include "ps2ur/anim.h"

#include "ps2ur/log.h"

namespace ps2ur {
namespace anim {

namespace {

bool g_initialized = false;

// Key record, 12 bytes, little-endian (docs/formats/p2b-container.md):
//   u16 time_norm  -- time / duration * 65535
//   u16 pad
//   i16 v[4]       -- quantised components (translation/scale use v[0..2])
inline constexpr uint32_t kKeyStride = 12;

uint16_t rd_u16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

int16_t rd_i16(const uint8_t* p)
{
    return static_cast<int16_t>(rd_u16(p));
}

float key_time(const uint8_t* key, float duration)
{
    return static_cast<float>(rd_u16(key)) * (1.0f / 65535.0f) * duration;
}

Vec3 key_vec3(const uint8_t* key, float quant_scale)
{
    const float s = quant_scale * (1.0f / 32767.0f);
    return Vec3{static_cast<float>(rd_i16(key + 4)) * s,
                static_cast<float>(rd_i16(key + 6)) * s,
                static_cast<float>(rd_i16(key + 8)) * s};
}

Quat key_quat(const uint8_t* key)
{
    const float s = 1.0f / 32767.0f;
    return Quat{static_cast<float>(rd_i16(key + 4)) * s,
                static_cast<float>(rd_i16(key + 6)) * s,
                static_cast<float>(rd_i16(key + 8)) * s,
                static_cast<float>(rd_i16(key + 10)) * s};
}

bool mask_has(const uint32_t* mask, uint32_t bone)
{
    return mask == nullptr || (mask[bone >> 5] & (1u << (bone & 31u))) != 0u;
}

Vec3 lerp3(Vec3 a, Vec3 b, float t)
{
    return Vec3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                a.z + (b.z - a.z) * t};
}

} // namespace

void rest_pose(const Skeleton& skeleton, Pose& out)
{
    for (uint32_t i = 0; i < skeleton.bone_count; ++i) {
        out.pos[i] = skeleton.bones[i].rest_pos;
        out.rot[i] = skeleton.bones[i].rest_rot;
        out.scale[i] = skeleton.bones[i].rest_scale;
    }
}

// ---- ClipPlayer -----------------------------------------------------------

void ClipPlayer::set_clip(const Clip* clip, float speed, bool loop)
{
    m_clip = clip;
    m_speed = speed;
    m_loop = loop;
    restart();
}

void ClipPlayer::restart()
{
    m_time = 0.0f;
    for (uint32_t i = 0; i < kMaxTracks; ++i) {
        m_cursor[i] = 0;
    }
}

void ClipPlayer::set_time(float t)
{
    // A backward seek invalidates every cursor; forward seeks are absorbed
    // by the walk in sample().
    if (t < m_time) {
        for (uint32_t i = 0; i < kMaxTracks; ++i) {
            m_cursor[i] = 0;
        }
    }
    m_time = t;
}

bool ClipPlayer::advance(float dt)
{
    if (m_clip == nullptr || m_clip->duration <= 0.0f) {
        return false;
    }
    m_time += dt * m_speed;
    bool ended = false;
    if (m_time >= m_clip->duration) {
        if (m_loop) {
            // Wrapping rewinds the cursors once per loop, not per frame.
            while (m_time >= m_clip->duration) {
                m_time -= m_clip->duration;
            }
            for (uint32_t i = 0; i < kMaxTracks; ++i) {
                m_cursor[i] = 0;
            }
        } else {
            m_time = m_clip->duration;
            ended = true;
        }
    } else if (m_time < 0.0f) {
        m_time = 0.0f;
        for (uint32_t i = 0; i < kMaxTracks; ++i) {
            m_cursor[i] = 0;
        }
    }
    return ended;
}

float ClipPlayer::normalized_time() const
{
    if (m_clip == nullptr || m_clip->duration <= 0.0f) {
        return 0.0f;
    }
    return m_time / m_clip->duration;
}

void ClipPlayer::sample(const Skeleton& skeleton, Pose& out)
{
    if (m_clip == nullptr || m_clip->keys == nullptr) {
        return;
    }
    const Clip& clip = *m_clip;
    for (uint32_t t = 0; t < clip.track_count && t < kMaxTracks; ++t) {
        const Track& track = clip.tracks[t];
        if (track.key_count == 0 || track.bone >= skeleton.bone_count) {
            continue;
        }
        const uint8_t* keys = clip.keys + track.key_first * kKeyStride;

        // Walk the cursor to the last key at or before m_time. Within a
        // play-through it only moves forward, which is the point (task 2).
        uint16_t cursor = m_cursor[t];
        if (cursor > track.key_count - 1u) {
            cursor = static_cast<uint16_t>(track.key_count - 1u);
        }
        while (cursor + 1u < track.key_count &&
               key_time(keys + (cursor + 1u) * kKeyStride, clip.duration) <= m_time) {
            ++cursor;
        }
        // Guards the frame after a wrap, where m_time precedes key 0.
        while (cursor > 0 &&
               key_time(keys + cursor * kKeyStride, clip.duration) > m_time) {
            --cursor;
        }
        m_cursor[t] = cursor;

        const uint8_t* k0 = keys + cursor * kKeyStride;
        const uint8_t* k1 = cursor + 1u < track.key_count
                                ? keys + (cursor + 1u) * kKeyStride
                                : k0;
        const float t0 = key_time(k0, clip.duration);
        const float t1 = key_time(k1, clip.duration);
        float u = 0.0f;
        if (t1 > t0) {
            u = (m_time - t0) / (t1 - t0);
            if (u < 0.0f) {
                u = 0.0f;
            }
            if (u > 1.0f) {
                u = 1.0f;
            }
        }

        switch (track.channel) {
            case kChannelRotation:
                out.rot[track.bone] = quat_slerp(key_quat(k0), key_quat(k1), u);
                break;
            case kChannelScale:
                out.scale[track.bone] = lerp3(key_vec3(k0, track.quant_scale),
                                              key_vec3(k1, track.quant_scale), u);
                break;
            default:
                out.pos[track.bone] = lerp3(key_vec3(k0, track.quant_scale),
                                            key_vec3(k1, track.quant_scale), u);
                break;
        }
    }
}

// ---- blending -------------------------------------------------------------

void blend_pose(Pose& a, const Pose& b, float t, const uint32_t* mask,
                uint32_t bone_count)
{
    if (t <= 0.0f) {
        return;
    }
    if (t > 1.0f) {
        t = 1.0f;
    }
    for (uint32_t i = 0; i < bone_count && i < kMaxBones; ++i) {
        if (!mask_has(mask, i)) {
            continue;
        }
        a.pos[i] = lerp3(a.pos[i], b.pos[i], t);
        a.rot[i] = quat_slerp(a.rot[i], b.rot[i], t);
        a.scale[i] = lerp3(a.scale[i], b.scale[i], t);
    }
}

void blend_additive(Pose& base, const Pose& additive, const Pose& reference,
                    float t, const uint32_t* mask, uint32_t bone_count)
{
    if (t <= 0.0f) {
        return;
    }
    if (t > 1.0f) {
        t = 1.0f;
    }
    for (uint32_t i = 0; i < bone_count && i < kMaxBones; ++i) {
        if (!mask_has(mask, i)) {
            continue;
        }
        // Delta relative to the reference pose, scaled by the weight.
        const Vec3 dp = sub(additive.pos[i], reference.pos[i]);
        base.pos[i] = add(base.pos[i], scale(dp, t));

        const Quat dq =
            quat_mul(additive.rot[i], quat_conjugate(reference.rot[i]));
        const Quat weighted = quat_slerp(quat_identity(), dq, t);
        base.rot[i] = quat_normalize(quat_mul(weighted, base.rot[i]));

        const Vec3 ds = sub(additive.scale[i], reference.scale[i]);
        base.scale[i] = add(base.scale[i], scale(ds, t));
    }
}

// ---- Animator -------------------------------------------------------------

void Animator::bind(const Skeleton* skeleton, const Controller* controller,
                    const Clip* clips, uint32_t clip_count)
{
    m_skeleton = skeleton;
    m_controller = controller;
    m_clips = clips;
    m_clip_count = clip_count;
    m_state = 0;
    m_blend_time = 0.0f;
    m_blend_duration = 0.0f;
    m_have_root_prev = false;
    if (skeleton != nullptr) {
        rest_pose(*skeleton, m_rest);
        m_pose = m_rest;
    }
    if (controller != nullptr && controller->state_count > 0) {
        play(0);
    }
}

// Common state-entry: binds the state's clip -- or, for a blend-tree
// state, primes the tree pair at phase zero.
void Animator::enter_state(uint32_t state_index)
{
    const StateDef& state = m_controller->states[state_index];
    m_state = state_index;
    m_have_root_prev = false;
    m_tree = m_controller->tree_for(state_index);
    m_tree_phase = 0.0f;
    m_tree_weight = 0.0f;
    if (state.clip < m_clip_count) {
        m_current.set_clip(&m_clips[state.clip], state.speed, state.loop);
    }
}

void Animator::play(uint32_t state_index)
{
    if (m_controller == nullptr || state_index >= m_controller->state_count) {
        return;
    }
    m_blend_duration = 0.0f;
    m_blend_time = 0.0f;
    enter_state(state_index);
}

void Animator::crossfade(uint32_t state_index, float duration)
{
    if (m_controller == nullptr || state_index >= m_controller->state_count) {
        return;
    }
    if (duration <= 0.0f) {
        play(state_index);
        return;
    }
    // The outgoing player keeps its own time and cursors, so the blend
    // samples both clips honestly instead of freezing one. When the
    // outgoing STATE was a tree, its dominant child carries the fade.
    m_previous = m_current;
    enter_state(state_index);
    m_blend_duration = duration;
    m_blend_time = 0.0f;
}

float Animator::blend_weight() const
{
    if (m_blend_duration <= 0.0f) {
        return 1.0f;
    }
    const float w = m_blend_time / m_blend_duration;
    return w > 1.0f ? 1.0f : w;
}

void Animator::set_float(uint32_t param_hash, float value)
{
    if (m_controller == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < m_controller->param_count; ++i) {
        if (m_controller->param_hash[i] == param_hash) {
            m_params[i] = value;
            return;
        }
    }
}

void Animator::set_bool(uint32_t param_hash, bool value)
{
    set_float(param_hash, value ? 1.0f : 0.0f);
}

void Animator::set_trigger(uint32_t param_hash)
{
    if (m_controller == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < m_controller->param_count; ++i) {
        if (m_controller->param_hash[i] == param_hash) {
            m_triggers[i] = true;
            return;
        }
    }
}

float Animator::param_value(uint8_t index) const
{
    return index < kMaxParams ? m_params[index] : 0.0f;
}

bool Animator::condition_met(const TransitionDef& transition)
{
    switch (transition.condition) {
        case kConditionBoolTrue:
            return param_value(transition.param) != 0.0f;
        case kConditionBoolFalse:
            return param_value(transition.param) == 0.0f;
        case kConditionFloatGreater:
            return param_value(transition.param) > transition.threshold;
        case kConditionFloatLess:
            return param_value(transition.param) < transition.threshold;
        case kConditionTrigger:
            if (transition.param < kMaxParams && m_triggers[transition.param]) {
                m_triggers[transition.param] = false; // consumed
                return true;
            }
            return false;
        default:
            return false; // exit-time is decided by the caller
    }
}

void Animator::apply_transitions(bool clip_ended)
{
    if (m_controller == nullptr || m_blend_duration > 0.0f) {
        return; // never start a transition while one is running
    }
    for (uint32_t i = 0; i < m_controller->transition_count; ++i) {
        const TransitionDef& transition = m_controller->transitions[i];
        if (transition.from != m_state) {
            continue;
        }
        const bool fires = transition.condition == kConditionExitTime
                               ? clip_ended
                               : condition_met(transition);
        if (fires) {
            crossfade(transition.to, transition.duration);
            return;
        }
    }
}

// Advances the current STATE's time: the single clip, or the blend-tree
// pair selected by the tree's parameter. Returns true when a non-looping
// state reached its end this step (the exit-time condition).
bool Animator::advance_state(float dt)
{
    if (m_tree < 0 || m_controller == nullptr) {
        return m_current.advance(dt);
    }
    const anim::BlendTreeDef& tree = m_controller->trees[m_tree];
    const StateDef& state = m_controller->states[m_state];

    // Segment selection: the pair of children whose thresholds bracket the
    // parameter, clamped at both ends.
    const float p = m_params[tree.param];
    uint32_t a = 0;
    while (a + 2u < tree.child_count && p >= tree.threshold[a + 1u]) {
        ++a;
    }
    const uint32_t b = a + 1u;
    const float span = tree.threshold[b] - tree.threshold[a];
    float w = span > 0.0001f ? (p - tree.threshold[a]) / span : 0.0f;
    if (w < 0.0f) w = 0.0f;
    if (w > 1.0f) w = 1.0f;
    m_tree_weight = w;

    const Clip* clip_a =
        tree.clip[a] < m_clip_count ? &m_clips[tree.clip[a]] : nullptr;
    const Clip* clip_b =
        tree.clip[b] < m_clip_count ? &m_clips[tree.clip[b]] : nullptr;
    if (clip_a == nullptr || clip_b == nullptr) {
        return m_current.advance(dt);
    }
    if (m_current.clip() != clip_a) {
        m_current.set_clip(clip_a, state.speed, state.loop);
    }
    if (m_tree_second.clip() != clip_b) {
        m_tree_second.set_clip(clip_b, state.speed, state.loop);
    }

    // Children play PHASE-LOCKED (Unity's 1D rule): one master phase, each
    // clip sampled at phase x its own duration. The phase advances at the
    // blended rate, so walk-to-run speeds up smoothly instead of snapping.
    const float blended_duration =
        clip_a->duration + (clip_b->duration - clip_a->duration) * w;
    bool ended = false;
    if (blended_duration > 0.0001f) {
        m_tree_phase += dt * state.speed / blended_duration;
        if (m_tree_phase >= 1.0f) {
            if (state.loop) {
                while (m_tree_phase >= 1.0f) {
                    m_tree_phase -= 1.0f;
                }
            } else {
                m_tree_phase = 1.0f;
                ended = true;
            }
        }
    }
    m_current.set_time(m_tree_phase * clip_a->duration);
    m_tree_second.set_time(m_tree_phase * clip_b->duration);
    return ended;
}

// Samples the current state's pose into 'out' (pre-seeded with rest): the
// single clip, or the tree pair blended by m_tree_weight.
void Animator::sample_state(Pose& out)
{
    m_current.sample(*m_skeleton, out);
    if (m_tree >= 0 && m_tree_weight > 0.0f &&
        m_tree_second.clip() != nullptr) {
        m_scratch2 = m_rest;
        m_tree_second.sample(*m_skeleton, m_scratch2);
        blend_pose(out, m_scratch2, m_tree_weight, nullptr,
                   m_skeleton->bone_count);
    }
}

void Animator::update(float dt)
{
    if (m_skeleton == nullptr) {
        return;
    }
    const uint32_t bones = m_skeleton->bone_count;

    const bool ended = advance_state(dt);
    if (m_blend_duration > 0.0f) {
        m_previous.advance(dt);
        m_blend_time += dt;
    }

    // Layer 0: the state machine's pose (clip or tree), with any crossfade
    // blended in.
    m_pose = m_rest;
    if (m_blend_duration > 0.0f) {
        m_previous.sample(*m_skeleton, m_pose);
        m_scratch = m_rest;
        sample_state(m_scratch);
        blend_pose(m_pose, m_scratch, blend_weight(), nullptr, bones);
        if (m_blend_time >= m_blend_duration) {
            m_blend_duration = 0.0f;
            m_blend_time = 0.0f;
        }
    } else {
        sample_state(m_pose);
    }

    // Extra layers: masked override or additive, on top of layer 0.
    for (uint32_t l = 1; l < kMaxLayers; ++l) {
        Layer& active = m_layers[l];
        if (!active.enabled || active.player.clip() == nullptr ||
            active.weight <= 0.0f) {
            continue;
        }
        active.player.advance(dt);
        m_scratch = m_rest;
        active.player.sample(*m_skeleton, m_scratch);
        const uint32_t* mask = active.masked ? active.mask : nullptr;
        if (active.additive) {
            blend_additive(m_pose, m_scratch, m_rest, active.weight, mask, bones);
        } else {
            blend_pose(m_pose, m_scratch, active.weight, mask, bones);
        }
    }

    // Root motion (M9 task 4): the root bone's per-frame delta becomes the
    // caller's to apply, and the pose pins the root so the character does
    // not travel twice.
    m_root_delta = Vec3{0, 0, 0};
    m_root_rot_delta = quat_identity();
    if (m_root_motion && bones > 0) {
        const Vec3 current_pos = m_pose.pos[0];
        const Quat current_rot = m_pose.rot[0];
        if (m_have_root_prev) {
            m_root_delta = sub(current_pos, m_root_prev);
            m_root_rot_delta =
                quat_mul(current_rot, quat_conjugate(m_root_prev_rot));
        }
        m_root_prev = current_pos;
        m_root_prev_rot = current_rot;
        m_have_root_prev = true;
        m_pose.pos[0] = m_skeleton->bones[0].rest_pos;
        m_pose.rot[0] = m_skeleton->bones[0].rest_rot;
    }

    compute_bone_world();
    apply_transitions(ended);
}

void Animator::compute_bone_world()
{
    if (m_skeleton == nullptr) {
        return;
    }
    // Parents strictly precede children (the loader enforces it), so one
    // linear pass resolves the whole skeleton.
    for (uint32_t i = 0; i < m_skeleton->bone_count; ++i) {
        const Mat4 local =
            mat4_trs(m_pose.pos[i], m_pose.rot[i], m_pose.scale[i]);
        const int32_t parent = m_skeleton->bones[i].parent;
        m_bone_world[i] =
            parent < 0 ? local : mat4_mul(m_bone_world[parent], local);
    }
}

Mat4 socket_world(const Animator& animator, const Socket& socket,
                  const Mat4& entity_world)
{
    const Mat4 bone = animator.bone_world(socket.bone);
    return mat4_mul(entity_world, mat4_mul(bone, socket.offset));
}

void build_palette(const Animator& animator, const uint16_t* table,
                   uint32_t table_count, Mat4* out_palette)
{
    const Skeleton* skeleton = animator.skeleton();
    if (skeleton == nullptr) {
        return;
    }
    for (uint32_t slot = 0; slot < table_count; ++slot) {
        const uint16_t bone = table[slot];
        if (bone >= skeleton->bone_count) {
            out_palette[slot] = mat4_identity();
            continue;
        }
        out_palette[slot] = mat4_mul(animator.bone_world(bone),
                                     skeleton->bones[bone].inverse_bind);
    }
}

// ---- partitioning ---------------------------------------------------------

uint32_t partition_triangles(const TriangleBones* triangles,
                             uint32_t triangle_count, uint32_t palette_size,
                             uint32_t max_triangles_per_batch,
                             PartitionBatch* out_batches, uint32_t max_batches)
{
    if (triangles == nullptr || out_batches == nullptr || palette_size == 0 ||
        palette_size > kMaxPaletteBones || max_triangles_per_batch == 0) {
        return 0;
    }

    uint32_t batch_count = 0;
    uint32_t i = 0;
    while (i < triangle_count) {
        if (batch_count >= max_batches) {
            return 0;
        }
        PartitionBatch& batch = out_batches[batch_count];
        batch.first_triangle = i;
        batch.triangle_count = 0;
        batch.bone_count = 0;

        while (i < triangle_count &&
               batch.triangle_count < max_triangles_per_batch) {
            const TriangleBones& tri = triangles[i];
            if (tri.count > palette_size) {
                return 0; // no partition can satisfy this triangle
            }
            // Count how many of this triangle's bones are new to the batch,
            // ignoring repeats within the triangle itself.
            uint32_t additions = 0;
            for (uint32_t b = 0; b < tri.count; ++b) {
                bool present = false;
                for (uint32_t s = 0; s < batch.bone_count && !present; ++s) {
                    present = batch.bones[s] == tri.bone[b];
                }
                if (present) {
                    continue;
                }
                bool already_counted = false;
                for (uint32_t p = 0; p < b && !already_counted; ++p) {
                    already_counted = tri.bone[p] == tri.bone[b];
                }
                if (!already_counted) {
                    ++additions;
                }
            }
            if (batch.bone_count + additions > palette_size) {
                break; // would overflow the palette: close this batch
            }
            for (uint32_t b = 0; b < tri.count; ++b) {
                bool present = false;
                for (uint32_t s = 0; s < batch.bone_count && !present; ++s) {
                    present = batch.bones[s] == tri.bone[b];
                }
                if (!present) {
                    batch.bones[batch.bone_count++] = tri.bone[b];
                }
            }
            ++batch.triangle_count;
            ++i;
        }

        if (batch.triangle_count == 0) {
            return 0; // no progress: unsatisfiable input
        }
        ++batch_count;
    }
    return batch_count;
}

bool validate_partition(const uint16_t* table, uint32_t table_count,
                        uint32_t skeleton_bone_count, const uint8_t* slots,
                        uint32_t slot_count, uint32_t palette_size)
{
    if (table == nullptr || table_count == 0 || table_count > palette_size) {
        return false;
    }
    for (uint32_t i = 0; i < table_count; ++i) {
        if (table[i] >= skeleton_bone_count) {
            return false;
        }
        for (uint32_t j = i + 1; j < table_count; ++j) {
            if (table[i] == table[j]) {
                return false; // duplicates waste slots and hide exporter bugs
            }
        }
    }
    for (uint32_t i = 0; i < slot_count; ++i) {
        if (slots[i] >= table_count) {
            return false;
        }
    }
    return true;
}

bool init()
{
    if (g_initialized) {
        return true;
    }
    g_initialized = true;
    log(LogLevel::Debug, "anim: init");
    return true;
}

void shutdown()
{
    if (!g_initialized) {
        return;
    }
    g_initialized = false;
    log(LogLevel::Debug, "anim: shutdown");
}

bool initialized() { return g_initialized; }

} // namespace anim
} // namespace ps2ur
