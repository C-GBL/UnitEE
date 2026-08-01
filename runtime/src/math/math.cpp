// Portable scalar implementations. VU0 macro-mode (COP2) variants replace
// these hot paths later (plan section 8: src/math). Single-precision only
// (plan section 3.1): sqrtf/sinf/cosf/tanf, never the double forms.
#include "ps2ur/math.h"

#include <cmath>

namespace ps2ur {

// ---- Vec3 ------------------------------------------------------------------

float length(Vec3 a)
{
    return sqrtf(length_sq(a));
}

Vec3 normalize(Vec3 a)
{
    const float len_sq = length_sq(a);
    if (len_sq <= 1.0e-12f) {
        return Vec3{0.0f, 0.0f, 0.0f};
    }
    const float inv = 1.0f / sqrtf(len_sq);
    return scale(a, inv);
}

// ---- Quat ------------------------------------------------------------------

Quat quat_mul(Quat a, Quat b)
{
    return Quat{
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

Quat quat_normalize(Quat q)
{
    const float len_sq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (len_sq <= 1.0e-12f) {
        return quat_identity();
    }
    const float inv = 1.0f / sqrtf(len_sq);
    return Quat{q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

Quat quat_from_axis_angle(Vec3 axis, float radians)
{
    const Vec3 n = normalize(axis);
    const float half = radians * 0.5f;
    const float s = sinf(half);
    return Quat{n.x * s, n.y * s, n.z * s, cosf(half)};
}

Vec3 quat_rotate(Quat q, Vec3 v)
{
    // v' = v + q.w * t + (u x t), where u = q.xyz and t = 2 * (u x v)
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = scale(cross(u, v), 2.0f);
    return add(v, add(scale(t, q.w), cross(u, t)));
}

// ---- Mat4 ------------------------------------------------------------------

Mat4 mat4_identity()
{
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 mat4_translate(Vec3 t)
{
    Mat4 r = mat4_identity();
    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}

Mat4 mat4_mul(const Mat4& a, const Mat4& b)
{
    Mat4 r{};
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = sum;
        }
    }
    return r;
}

Vec4 mat4_mul_vec4(const Mat4& a, Vec4 v)
{
    return Vec4{
        a.m[0] * v.x + a.m[4] * v.y + a.m[8] * v.z + a.m[12] * v.w,
        a.m[1] * v.x + a.m[5] * v.y + a.m[9] * v.z + a.m[13] * v.w,
        a.m[2] * v.x + a.m[6] * v.y + a.m[10] * v.z + a.m[14] * v.w,
        a.m[3] * v.x + a.m[7] * v.y + a.m[11] * v.z + a.m[15] * v.w,
    };
}

Mat4 mat4_perspective(float fovy_radians, float aspect, float znear, float zfar)
{
    const float f = 1.0f / tanf(fovy_radians * 0.5f);
    Mat4 r{};
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (zfar + znear) / (znear - zfar);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * zfar * znear) / (znear - zfar);
    return r;
}


Mat4 mat4_from_quat(Quat q)
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    Mat4 r = mat4_identity();
    r.m[0] = 1.0f - 2.0f * (y * y + z * z);
    r.m[1] = 2.0f * (x * y + z * w);
    r.m[2] = 2.0f * (x * z - y * w);
    r.m[4] = 2.0f * (x * y - z * w);
    r.m[5] = 1.0f - 2.0f * (x * x + z * z);
    r.m[6] = 2.0f * (y * z + x * w);
    r.m[8] = 2.0f * (x * z + y * w);
    r.m[9] = 2.0f * (y * z - x * w);
    r.m[10] = 1.0f - 2.0f * (x * x + y * y);
    return r;
}

Mat4 mat4_trs(Vec3 t, Quat r, Vec3 s)
{
    Mat4 m = mat4_from_quat(r);
    // Scale the basis columns, then set the translation column.
    for (int c = 0; c < 3; ++c) {
        const float k = (c == 0) ? s.x : (c == 1) ? s.y : s.z;
        m.m[c * 4 + 0] *= k;
        m.m[c * 4 + 1] *= k;
        m.m[c * 4 + 2] *= k;
    }
    m.m[12] = t.x;
    m.m[13] = t.y;
    m.m[14] = t.z;
    return m;
}

Mat4 mat4_rigid_inverse(const Mat4& m)
{
    Mat4 r = mat4_identity();
    // Transpose the 3x3 rotation.
    for (int c = 0; c < 3; ++c) {
        for (int row = 0; row < 3; ++row) {
            r.m[c * 4 + row] = m.m[row * 4 + c];
        }
    }
    // New translation = -R^T * t.
    const float tx = m.m[12], ty = m.m[13], tz = m.m[14];
    r.m[12] = -(r.m[0] * tx + r.m[4] * ty + r.m[8] * tz);
    r.m[13] = -(r.m[1] * tx + r.m[5] * ty + r.m[9] * tz);
    r.m[14] = -(r.m[2] * tx + r.m[6] * ty + r.m[10] * tz);
    return r;
}

} // namespace ps2ur
