// Math unit tests. Epsilon reflects single-precision-only math (plan 3.1).
#include "ps2ur/math.h"

#include <gtest/gtest.h>

using namespace ps2ur;

static constexpr float kEps = 1.0e-5f;

TEST(Vec3, AddSubScale)
{
    const Vec3 a{1.0f, 2.0f, 3.0f};
    const Vec3 b{4.0f, -5.0f, 6.0f};

    const Vec3 s = add(a, b);
    EXPECT_NEAR(s.x, 5.0f, kEps);
    EXPECT_NEAR(s.y, -3.0f, kEps);
    EXPECT_NEAR(s.z, 9.0f, kEps);

    const Vec3 d = sub(a, b);
    EXPECT_NEAR(d.x, -3.0f, kEps);
    EXPECT_NEAR(d.y, 7.0f, kEps);
    EXPECT_NEAR(d.z, -3.0f, kEps);

    const Vec3 sc = scale(a, 2.0f);
    EXPECT_NEAR(sc.x, 2.0f, kEps);
    EXPECT_NEAR(sc.y, 4.0f, kEps);
    EXPECT_NEAR(sc.z, 6.0f, kEps);
}

TEST(Vec3, DotCross)
{
    const Vec3 x{1.0f, 0.0f, 0.0f};
    const Vec3 y{0.0f, 1.0f, 0.0f};

    EXPECT_NEAR(dot(x, y), 0.0f, kEps);
    EXPECT_NEAR(dot(x, x), 1.0f, kEps);

    const Vec3 z = cross(x, y);
    EXPECT_NEAR(z.x, 0.0f, kEps);
    EXPECT_NEAR(z.y, 0.0f, kEps);
    EXPECT_NEAR(z.z, 1.0f, kEps);
}

TEST(Vec3, NormalizeAndZeroInput)
{
    const Vec3 v{3.0f, 4.0f, 0.0f};
    EXPECT_NEAR(length(v), 5.0f, kEps);

    const Vec3 n = normalize(v);
    EXPECT_NEAR(length(n), 1.0f, kEps);
    EXPECT_NEAR(n.x, 0.6f, kEps);
    EXPECT_NEAR(n.y, 0.8f, kEps);

    const Vec3 zero = normalize(Vec3{0.0f, 0.0f, 0.0f});
    EXPECT_NEAR(zero.x, 0.0f, kEps);
    EXPECT_NEAR(zero.y, 0.0f, kEps);
    EXPECT_NEAR(zero.z, 0.0f, kEps);
}

TEST(Vec2Vec4, Basics)
{
    const Vec2 a2 = add(Vec2{1.0f, 2.0f}, Vec2{3.0f, 4.0f});
    EXPECT_NEAR(a2.x, 4.0f, kEps);
    EXPECT_NEAR(a2.y, 6.0f, kEps);
    EXPECT_NEAR(dot(Vec2{1.0f, 0.0f}, Vec2{0.0f, 1.0f}), 0.0f, kEps);

    const Vec4 a4 = scale(Vec4{1.0f, 2.0f, 3.0f, 4.0f}, 0.5f);
    EXPECT_NEAR(a4.w, 2.0f, kEps);
    EXPECT_NEAR(dot(Vec4{1.0f, 2.0f, 3.0f, 4.0f}, Vec4{1.0f, 1.0f, 1.0f, 1.0f}),
                10.0f, kEps);
}

TEST(Quat, IdentityRotateIsNoop)
{
    const Vec3 v{1.0f, 2.0f, 3.0f};
    const Vec3 r = quat_rotate(quat_identity(), v);
    EXPECT_NEAR(r.x, v.x, kEps);
    EXPECT_NEAR(r.y, v.y, kEps);
    EXPECT_NEAR(r.z, v.z, kEps);
}

TEST(Quat, AxisAngleRotation)
{
    // 90 degrees about +Z maps +X to +Y.
    const Quat q = quat_from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, kPi * 0.5f);
    const Vec3 r = quat_rotate(q, Vec3{1.0f, 0.0f, 0.0f});
    EXPECT_NEAR(r.x, 0.0f, kEps);
    EXPECT_NEAR(r.y, 1.0f, kEps);
    EXPECT_NEAR(r.z, 0.0f, kEps);
}

TEST(Quat, MulComposesRotations)
{
    // Two 45-degree rotations about Z compose to 90 degrees.
    const Quat h = quat_from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, kPi * 0.25f);
    const Quat q = quat_mul(h, h);
    const Vec3 r = quat_rotate(q, Vec3{1.0f, 0.0f, 0.0f});
    EXPECT_NEAR(r.x, 0.0f, kEps);
    EXPECT_NEAR(r.y, 1.0f, kEps);
    EXPECT_NEAR(r.z, 0.0f, kEps);
}

TEST(Quat, NormalizeUnitLength)
{
    const Quat q = quat_normalize(Quat{1.0f, 2.0f, 3.0f, 4.0f});
    const float len_sq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    EXPECT_NEAR(len_sq, 1.0f, kEps);
}

TEST(Mat4, IdentityIsNeutral)
{
    const Mat4 i = mat4_identity();
    const Vec4 p{1.0f, 2.0f, 3.0f, 1.0f};
    const Vec4 r = mat4_mul_vec4(i, p);
    EXPECT_NEAR(r.x, p.x, kEps);
    EXPECT_NEAR(r.y, p.y, kEps);
    EXPECT_NEAR(r.z, p.z, kEps);
    EXPECT_NEAR(r.w, p.w, kEps);

    const Mat4 ii = mat4_mul(i, i);
    for (int k = 0; k < 16; ++k) {
        EXPECT_NEAR(ii.m[k], i.m[k], kEps);
    }
}

TEST(Mat4, TranslateAndCompose)
{
    const Mat4 t1 = mat4_translate(Vec3{1.0f, 2.0f, 3.0f});
    const Mat4 t2 = mat4_translate(Vec3{10.0f, 20.0f, 30.0f});

    const Vec4 p = mat4_mul_vec4(t1, Vec4{0.0f, 0.0f, 0.0f, 1.0f});
    EXPECT_NEAR(p.x, 1.0f, kEps);
    EXPECT_NEAR(p.y, 2.0f, kEps);
    EXPECT_NEAR(p.z, 3.0f, kEps);

    const Mat4 t12 = mat4_mul(t1, t2);
    const Vec4 q = mat4_mul_vec4(t12, Vec4{0.0f, 0.0f, 0.0f, 1.0f});
    EXPECT_NEAR(q.x, 11.0f, kEps);
    EXPECT_NEAR(q.y, 22.0f, kEps);
    EXPECT_NEAR(q.z, 33.0f, kEps);

    // Direction vectors (w = 0) are unaffected by translation.
    const Vec4 dir = mat4_mul_vec4(t1, Vec4{0.0f, 0.0f, 1.0f, 0.0f});
    EXPECT_NEAR(dir.x, 0.0f, kEps);
    EXPECT_NEAR(dir.z, 1.0f, kEps);
}

TEST(Mat4, PerspectiveShape)
{
    // fovy 90 deg, aspect 1: f = 1.
    const Mat4 p = mat4_perspective(kPi * 0.5f, 1.0f, 0.1f, 100.0f);
    EXPECT_NEAR(p.m[0], 1.0f, kEps);
    EXPECT_NEAR(p.m[5], 1.0f, kEps);
    EXPECT_NEAR(p.m[11], -1.0f, kEps);
    EXPECT_NEAR(p.m[15], 0.0f, kEps);

    // A point on the near plane maps to clip z/w = -1; far plane to +1.
    const Vec4 near_pt = mat4_mul_vec4(p, Vec4{0.0f, 0.0f, -0.1f, 1.0f});
    EXPECT_NEAR(near_pt.z / near_pt.w, -1.0f, 1.0e-4f);
    const Vec4 far_pt = mat4_mul_vec4(p, Vec4{0.0f, 0.0f, -100.0f, 1.0f});
    EXPECT_NEAR(far_pt.z / far_pt.w, 1.0f, 1.0e-4f);
}
