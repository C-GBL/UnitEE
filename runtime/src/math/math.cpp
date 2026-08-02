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
#if defined(PS2UR_PLATFORM_PS2)
    // VU0 macro mode (plan M8 task 1): r_col = A * b_col via the broadcast
    // MAC pipeline -- one lq + four MADDs + one sq per column. Mat4 is
    // alignas(16) so lqc2/sqc2 are safe. VU MADD keeps the accumulate in
    // ACC with per-step rounding like the scalar version, but the fused
    // multiply-add rounds once where scalar rounds twice: bit-level results
    // differ from host (goldens are per-platform anyway, plan 12).
    Mat4 r;
    asm volatile(
        "lqc2   $vf1,  0(%1)\n"
        "lqc2   $vf2, 16(%1)\n"
        "lqc2   $vf3, 32(%1)\n"
        "lqc2   $vf4, 48(%1)\n"
        "lqc2   $vf5,  0(%2)\n"
        "lqc2   $vf6, 16(%2)\n"
        "lqc2   $vf7, 32(%2)\n"
        "lqc2   $vf8, 48(%2)\n"
        "vmulax.xyzw  $ACC, $vf1, $vf5x\n"
        "vmadday.xyzw $ACC, $vf2, $vf5y\n"
        "vmaddaz.xyzw $ACC, $vf3, $vf5z\n"
        "vmaddw.xyzw  $vf9, $vf4, $vf5w\n"
        "vmulax.xyzw  $ACC, $vf1, $vf6x\n"
        "vmadday.xyzw $ACC, $vf2, $vf6y\n"
        "vmaddaz.xyzw $ACC, $vf3, $vf6z\n"
        "vmaddw.xyzw  $vf10, $vf4, $vf6w\n"
        "vmulax.xyzw  $ACC, $vf1, $vf7x\n"
        "vmadday.xyzw $ACC, $vf2, $vf7y\n"
        "vmaddaz.xyzw $ACC, $vf3, $vf7z\n"
        "vmaddw.xyzw  $vf11, $vf4, $vf7w\n"
        "vmulax.xyzw  $ACC, $vf1, $vf8x\n"
        "vmadday.xyzw $ACC, $vf2, $vf8y\n"
        "vmaddaz.xyzw $ACC, $vf3, $vf8z\n"
        "vmaddw.xyzw  $vf12, $vf4, $vf8w\n"
        "sqc2   $vf9,   0(%0)\n"
        "sqc2   $vf10, 16(%0)\n"
        "sqc2   $vf11, 32(%0)\n"
        "sqc2   $vf12, 48(%0)\n"
        :
        : "r"(r.m), "r"(a.m), "r"(b.m)
        : "memory");
    return r;
#else
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
#endif
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

Quat quat_slerp(Quat a, Quat b, float t)
{
    float cosine = quat_dot(a, b);
    if (cosine < 0.0f) {
        // Same rotation, opposite representation: take the short arc.
        b = Quat{-b.x, -b.y, -b.z, -b.w};
        cosine = -cosine;
    }
    if (cosine > 0.9995f) {
        // Nearly parallel: the sine denominator collapses, and lerp is
        // indistinguishable at this angle anyway.
        return quat_normalize(Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                                   a.z + (b.z - a.z) * t,
                                   a.w + (b.w - a.w) * t});
    }
    const float theta = acosf(cosine);
    const float sin_theta = sinf(theta);
    const float wa = sinf((1.0f - t) * theta) / sin_theta;
    const float wb = sinf(t * theta) / sin_theta;
    return Quat{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb,
                a.w * wa + b.w * wb};
}

Mat4 mat4_ortho(float half_w, float half_h, float znear, float zfar)
{
    Mat4 r{};
    r.m[0] = 1.0f / half_w;
    r.m[5] = 1.0f / half_h;
    r.m[10] = 2.0f / (znear - zfar);
    r.m[14] = (zfar + znear) / (znear - zfar);
    r.m[15] = 1.0f;
    return r;
}

FrustumPlanes frustum_from_viewproj(const Mat4& vp)
{
    // Gribb/Hartmann on a column-major matrix: row_i(vp) combinations.
    // row r of vp = (m[r], m[4+r], m[8+r], m[12+r]).
    auto row = [&vp](int r) {
        return Vec4{vp.m[r], vp.m[4 + r], vp.m[8 + r], vp.m[12 + r]};
    };
    const Vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);

    FrustumPlanes f;
    f.plane[0] = add(r3, r0);  // left
    f.plane[1] = sub(r3, r0);  // right
    f.plane[2] = add(r3, r1);  // bottom
    f.plane[3] = sub(r3, r1);  // top
    f.plane[4] = add(r3, r2);  // near
    f.plane[5] = sub(r3, r2);  // far
    for (int i = 0; i < 6; ++i) {
        const Vec3 n{f.plane[i].x, f.plane[i].y, f.plane[i].z};
        const float len = length(n);
        if (len > 1e-12f) {
            const float inv = 1.0f / len;
            f.plane[i] = Vec4{n.x * inv, n.y * inv, n.z * inv,
                              f.plane[i].w * inv};
        }
    }
    return f;
}

bool frustum_culls_sphere(const FrustumPlanes& f, Vec3 center, float radius)
{
    for (int i = 0; i < 6; ++i) {
        const float d = f.plane[i].x * center.x + f.plane[i].y * center.y +
                        f.plane[i].z * center.z + f.plane[i].w;
        if (d < -radius) {
            return true;
        }
    }
    return false;
}

} // namespace ps2ur
