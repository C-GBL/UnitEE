// anim: skeletons, quantised clip sampling, crossfade/additive/layered
// blending, a baked state machine, root motion, sockets, and the VU1 bone
// palette (plan section 9, M9; formats in docs/formats/p2b-container.md).
//
// Design notes that matter on this hardware:
//
//  - Sampling is CURSOR-based (M9 task 2): every track remembers the key it
//    stopped at and walks forward from there. Binary-searching 24 bones x 3
//    channels every frame would cost more than the blending does.
//  - Keys are 12-byte quantised records read straight out of the .p2b buffer
//    (zero-copy); rotations are 4x16-bit, translations/scales 16-bit with a
//    per-track dequantisation scale.
//  - Poses are LOCAL TRS. Model matrices are composed once per frame in a
//    parent-before-child pass, exactly like the scene graph, then combined
//    with the inverse bind pose into the palette the VU1 program indexes.
//  - No doubles, no allocation, fixed-size storage: an Animator is a plain
//    object the scene owns.
#pragma once

#include "ps2ur/math.h"

#include <cstdint>

namespace ps2ur {
namespace anim {

inline constexpr uint32_t kMaxBones = 64;
// The VU1 data-memory palette (plan M9 task 3). 24 bones x 4 qwords sits at
// qwords 18..113, leaving vertices from 114.
inline constexpr uint32_t kMaxPaletteBones = 24;
inline constexpr uint32_t kMaxTracks = 256;
inline constexpr uint32_t kMaxClips = 8;
inline constexpr uint32_t kMaxStates = 16;
inline constexpr uint32_t kMaxTransitions = 32;
inline constexpr uint32_t kMaxParams = 8;
inline constexpr uint32_t kMaxLayers = 2;
inline constexpr uint32_t kMaskWords = (kMaxBones + 31) / 32;

inline constexpr uint8_t kChannelTranslation = 0;
inline constexpr uint8_t kChannelRotation = 1;
inline constexpr uint8_t kChannelScale = 2;

// Transition conditions (baked from the Animator controller, M9 task 1).
inline constexpr uint8_t kConditionExitTime = 0; // fires when the clip ends
inline constexpr uint8_t kConditionBoolTrue = 1;
inline constexpr uint8_t kConditionBoolFalse = 2;
inline constexpr uint8_t kConditionFloatGreater = 3;
inline constexpr uint8_t kConditionFloatLess = 4;
inline constexpr uint8_t kConditionTrigger = 5;

struct Bone {
    int32_t parent = -1; // strictly less than this bone's index
    uint32_t name_hash = 0;
    Mat4 inverse_bind = mat4_identity(); // mesh space -> bone space
    Vec3 rest_pos{0, 0, 0};
    Quat rest_rot{0, 0, 0, 1};
    Vec3 rest_scale{1, 1, 1};
};

struct Skeleton {
    Bone bones[kMaxBones];
    uint32_t bone_count = 0;
};

// One animated channel of one bone. 'key_first' indexes the clip's key
// stream; keys are the 12-byte records described in the format doc.
struct Track {
    uint16_t bone = 0;
    uint8_t channel = kChannelTranslation;
    uint8_t pad = 0;
    uint16_t key_count = 0;
    uint16_t pad2 = 0;
    uint32_t key_first = 0;
    float quant_scale = 1.0f; // value = raw * quant_scale / 32767
};

struct Clip {
    uint32_t name_hash = 0;
    float duration = 0.0f; // seconds
    bool loop = false;
    const uint8_t* keys = nullptr; // 12-byte records, inside the .p2b buffer
    uint32_t key_count = 0;        // total, for bounds checks
    Track tracks[kMaxTracks];
    uint32_t track_count = 0;
};

struct StateDef {
    uint32_t name_hash = 0;
    uint16_t clip = 0;
    float speed = 1.0f;
    bool loop = true;
};

struct TransitionDef {
    uint16_t from = 0;
    uint16_t to = 0;
    float duration = 0.0f; // crossfade seconds
    uint8_t condition = kConditionExitTime;
    uint8_t param = 0;
    float threshold = 0.0f;
};

struct Controller {
    StateDef states[kMaxStates];
    uint32_t state_count = 0;
    TransitionDef transitions[kMaxTransitions];
    uint32_t transition_count = 0;
    uint32_t param_hash[kMaxParams] = {};
    uint32_t param_count = 0;
};

// A local-space pose: what sampling produces and blending combines.
struct Pose {
    Vec3 pos[kMaxBones];
    Quat rot[kMaxBones];
    Vec3 scale[kMaxBones];
};

// Fills 'out' with the skeleton's rest pose (the identity of blending).
void rest_pose(const Skeleton& skeleton, Pose& out);

// Cursor-cached clip evaluation. One per concurrently playing clip; the
// crossfade in Animator uses two.
class ClipPlayer {
public:
    void set_clip(const Clip* clip, float speed = 1.0f, bool loop = true);
    const Clip* clip() const { return m_clip; }

    void restart();
    // Advances by dt (seconds) and returns true when a non-looping clip
    // ENDED this step -- the exit-time transition condition.
    bool advance(float dt);

    float time() const { return m_time; }
    void set_time(float t);
    float normalized_time() const;

    // Samples into 'out'. Bones with no track keep whatever 'out' holds, so
    // callers seed it with the rest pose (or a lower layer's result).
    void sample(const Skeleton& skeleton, Pose& out);

private:
    const Clip* m_clip = nullptr;
    float m_time = 0.0f;
    float m_speed = 1.0f;
    bool m_loop = true;
    // Per-track key cursor: sampling walks forward from here and never
    // binary-searches (M9 task 2).
    uint16_t m_cursor[kMaxTracks] = {};
};

// Blends 'b' into 'a' by weight t (0 = a, 1 = b) for bones selected by the
// mask (nullptr = every bone). Rotations use shortest-arc slerp.
void blend_pose(Pose& a, const Pose& b, float t, const uint32_t* mask,
                uint32_t bone_count);

// Applies 'additive' relative to 'reference' on top of 'base' by weight t:
// the standard additive-layer rule (delta = additive - reference).
void blend_additive(Pose& base, const Pose& additive, const Pose& reference,
                    float t, const uint32_t* mask, uint32_t bone_count);

struct Layer {
    ClipPlayer player;
    float weight = 1.0f;
    bool additive = false;
    bool enabled = false;
    bool masked = false; // false = every bone
    uint32_t mask[kMaskWords] = {};
};

// Baked-controller playback: states, crossfades, parameters, layers, root
// motion (M9 tasks 2 and 4).
class Animator {
public:
    void bind(const Skeleton* skeleton, const Controller* controller,
              const Clip* clips, uint32_t clip_count);

    bool valid() const { return m_skeleton != nullptr; }
    const Skeleton* skeleton() const { return m_skeleton; }

    // Jumps to a state with no blend.
    void play(uint32_t state_index);
    // Starts a crossfade to a state over 'duration' seconds.
    void crossfade(uint32_t state_index, float duration);
    uint32_t state() const { return m_state; }
    bool blending() const { return m_blend_duration > 0.0f; }
    float blend_weight() const;

    void set_float(uint32_t param_hash, float value);
    void set_bool(uint32_t param_hash, bool value);
    void set_trigger(uint32_t param_hash);

    // Extra layers (index >= 1); layer 0 is the state machine's own output.
    Layer& layer(uint32_t index) { return m_layers[index]; }

    // Advances time, evaluates every layer, applies transitions, and leaves
    // the result in pose() with model matrices in bone_world(). Root motion
    // (M9 task 4) is extracted from the root bone into root_motion_delta()
    // and removed from the pose, so the caller drives the entity with it.
    void update(float dt);

    const Pose& pose() const { return m_pose; }

    void set_root_motion(bool enabled) { m_root_motion = enabled; }
    Vec3 root_motion_delta() const { return m_root_delta; }
    Quat root_rotation_delta() const { return m_root_rot_delta; }

    // Model-space bone matrices for the current pose (parent-before-child).
    // Valid after update(); sockets and the palette read these.
    const Mat4& bone_world(uint32_t bone) const { return m_bone_world[bone]; }
    void compute_bone_world();

private:
    void apply_transitions(bool clip_ended);
    bool condition_met(const TransitionDef& transition);
    float param_value(uint8_t index) const;

    const Skeleton* m_skeleton = nullptr;
    const Controller* m_controller = nullptr;
    const Clip* m_clips = nullptr;
    uint32_t m_clip_count = 0;

    uint32_t m_state = 0;
    ClipPlayer m_current;
    ClipPlayer m_previous;
    float m_blend_time = 0.0f;
    float m_blend_duration = 0.0f;

    float m_params[kMaxParams] = {};
    bool m_triggers[kMaxParams] = {};

    Layer m_layers[kMaxLayers];

    Pose m_pose;
    Pose m_scratch;
    Pose m_rest;
    Mat4 m_bone_world[kMaxBones];

    bool m_root_motion = false;
    bool m_have_root_prev = false;
    Vec3 m_root_prev{0, 0, 0};
    Quat m_root_prev_rot{0, 0, 0, 1};
    Vec3 m_root_delta{0, 0, 0};
    Quat m_root_rot_delta{0, 0, 0, 1};
};

// A gameplay attachment point (M9 task 5): a fixed offset in a bone's space.
struct Socket {
    uint32_t name_hash = 0;
    uint16_t bone = 0;
    Mat4 offset = mat4_identity();
};

// World matrix of a socket, given the entity's world matrix.
Mat4 socket_world(const Animator& animator, const Socket& socket,
                  const Mat4& entity_world);

// Builds the VU1 palette for one batch: palette[slot] = bone_world[table
// [slot]] * inverse_bind[table[slot]], i.e. mesh space -> posed mesh space.
// 'table' is the batch's bone list (<= kMaxPaletteBones).
void build_palette(const Animator& animator, const uint16_t* table,
                   uint32_t table_count, Mat4* out_palette);

// ---- offline bone partitioning (M9 task 3) --------------------------------
//
// A skinned batch may reference at most kMaxPaletteBones distinct bones, so
// triangles are grouped into batches whose combined influence sets fit. This
// is the SPECIFICATION of the greedy algorithm the exporter implements: walk
// triangles in order, keep adding to the current batch while its bone set
// stays inside the palette and the triangle budget, otherwise start a new
// batch. Sorting triangles by influence set first (the exporter does) turns
// the greedy walk into few, dense batches.
//
// The runtime never partitions -- it VALIDATES what the exporter produced
// (validate_partition), so an exporter bug fails loudly at load instead of
// drawing a character inside out.

struct TriangleBones {
    uint16_t bone[12]; // up to 4 influences x 3 vertices
    uint8_t count = 0; // distinct bones actually used
};

struct PartitionBatch {
    uint32_t first_triangle = 0;
    uint32_t triangle_count = 0;
    uint16_t bones[kMaxPaletteBones];
    uint32_t bone_count = 0;
};

// Greedy partition. Returns the number of batches written, or 0 when a
// triangle needs more than 'palette_size' bones (unsatisfiable) or the
// output array is too small.
uint32_t partition_triangles(const TriangleBones* triangles,
                             uint32_t triangle_count, uint32_t palette_size,
                             uint32_t max_triangles_per_batch,
                             PartitionBatch* out_batches,
                             uint32_t max_batches);

// Checks one exported batch: table size within the palette, no duplicate
// bones, every bone index inside the skeleton, every local slot used by the
// batch's vertices inside the table.
bool validate_partition(const uint16_t* table, uint32_t table_count,
                        uint32_t skeleton_bone_count, const uint8_t* slots,
                        uint32_t slot_count, uint32_t palette_size);

bool init();
void shutdown();
bool initialized();

} // namespace anim
} // namespace ps2ur
