// Math types: Vec2/Vec3/Vec4/Quat/Mat4 as plain structs + free functions.
//
// SINGLE-PRECISION ONLY. The EE FPU has no double-precision hardware; every
// 'double' compiles to a libgcc soft-float call, 20-100x slower (plan section
// 3.1). No doubles anywhere in runtime code; all literals carry an 'f' suffix.
//
// These are portable scalar C++ implementations. VU0 macro-mode (COP2) variants
// replace the hot paths later (plan section 8: src/math -- "VU0 macro-mode
// vec/mat, transcendentals"; section 3.2).
#pragma once

namespace ps2ur {

inline constexpr float kPi = 3.14159265358979323846f;

struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };
struct Vec4 { float x, y, z, w; };
struct Quat { float x, y, z, w; }; // w is the scalar part

// Column-major 4x4: element (row r, column c) lives at m[c * 4 + r].
// Transforms apply to column vectors: p' = M * p.
// 16-byte aligned since M8: the VU0 macro-mode multiply loads columns with
// lqc2, which faults on unaligned addresses.
struct alignas(16) Mat4 { float m[16]; };
static_assert(sizeof(Mat4) == 64, "Mat4 must stay 16 floats");

// ---- Vec2 ------------------------------------------------------------------
inline Vec2 add(Vec2 a, Vec2 b)      { return Vec2{a.x + b.x, a.y + b.y}; }
inline Vec2 sub(Vec2 a, Vec2 b)      { return Vec2{a.x - b.x, a.y - b.y}; }
inline Vec2 scale(Vec2 a, float s)   { return Vec2{a.x * s, a.y * s}; }
inline float dot(Vec2 a, Vec2 b)     { return a.x * b.x + a.y * b.y; }

// ---- Vec3 ------------------------------------------------------------------
inline Vec3 add(Vec3 a, Vec3 b)      { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 sub(Vec3 a, Vec3 b)      { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 scale(Vec3 a, float s)   { return Vec3{a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b)     { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return Vec3{a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x};
}
inline float length_sq(Vec3 a)       { return dot(a, a); }
float length(Vec3 a);
Vec3 normalize(Vec3 a); // returns the zero vector for near-zero input

// ---- Vec4 ------------------------------------------------------------------
inline Vec4 add(Vec4 a, Vec4 b)      { return Vec4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
inline Vec4 sub(Vec4 a, Vec4 b)      { return Vec4{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
inline Vec4 scale(Vec4 a, float s)   { return Vec4{a.x * s, a.y * s, a.z * s, a.w * s}; }
inline float dot(Vec4 a, Vec4 b)     { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

// ---- Quat ------------------------------------------------------------------
inline Quat quat_identity()          { return Quat{0.0f, 0.0f, 0.0f, 1.0f}; }
inline float quat_dot(Quat a, Quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline Quat quat_conjugate(Quat q)   { return Quat{-q.x, -q.y, -q.z, q.w}; }
Quat quat_mul(Quat a, Quat b);       // Hamilton product: applies b, then a
Quat quat_normalize(Quat q);
Quat quat_from_axis_angle(Vec3 axis, float radians);
Vec3 quat_rotate(Quat q, Vec3 v);
// Shortest-arc interpolation (M9): flips b when the dot product is negative
// so a blend never takes the long way round. Falls back to normalised lerp
// for nearly parallel inputs, where the slerp denominator collapses.
Quat quat_slerp(Quat a, Quat b, float t);

// ---- Mat4 ------------------------------------------------------------------
Mat4 mat4_identity();
Mat4 mat4_translate(Vec3 t);
Mat4 mat4_mul(const Mat4& a, const Mat4& b);      // a * b (b applies first)
Vec4 mat4_mul_vec4(const Mat4& a, Vec4 v);

// Right-handed, symmetric frustum, clip-space z in [-1, 1] (GL convention).
// TODO(spec missing: section 9): final GS depth-range/viewport convention may
// swap this to a [0, 1] or fixed-point Z mapping at the VU1 stage.
Mat4 mat4_perspective(float fovy_radians, float aspect, float znear, float zfar);

// Right-handed orthographic, clip-space z in [-1, 1], symmetric about the
// view axis: half_h is Unity's orthographicSize, half_w = half_h * aspect.
Mat4 mat4_ortho(float half_w, float half_h, float znear, float zfar);

// ---- Frustum culling (M8 task 3) -------------------------------------------
//
// Six planes extracted from a view-projection matrix (Gribb/Hartmann),
// normalized, pointing INWARD: a point p is inside when dot(n, p) + d >= 0
// for all six.
struct FrustumPlanes {
    Vec4 plane[6]; // xyz = normal, w = d
};

FrustumPlanes frustum_from_viewproj(const Mat4& viewproj);

// True if the sphere is fully outside any plane (cull it). Conservative:
// spheres straddling planes are kept.
bool frustum_culls_sphere(const FrustumPlanes& f, Vec3 center, float radius);

// Rotation matrix from a unit quaternion (x,y,z,w).
Mat4 mat4_from_quat(Quat q);
// Compose translate * rotate * scale -- the standard local transform.
Mat4 mat4_trs(Vec3 t, Quat r, Vec3 s);
// Inverse of a RIGID transform (rotation + translation, unit scale): the
// camera view matrix from a camera world matrix. Cheap: transpose + dot.
Mat4 mat4_rigid_inverse(const Mat4& m);

} // namespace ps2ur
