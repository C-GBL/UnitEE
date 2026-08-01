using System;

namespace UnityEngine
{
    public struct Quaternion : IEquatable<Quaternion>
    {
        public float x;
        public float y;
        public float z;
        public float w;

        public const float kEpsilon = 1e-6f;

        public Quaternion(float x, float y, float z, float w) { this.x = x; this.y = y; this.z = z; this.w = w; }

        public static Quaternion identity => new Quaternion(0f, 0f, 0f, 1f);

        public static Quaternion operator *(Quaternion a, Quaternion b) => new Quaternion(
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y + a.y * b.w + a.z * b.x - a.x * b.z,
            a.w * b.z + a.z * b.w + a.x * b.y - a.y * b.x,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);

        public static Vector3 operator *(Quaternion q, Vector3 v)
        {
            // v' = v + 2w(q_v x v) + 2(q_v x (q_v x v))
            float tx = 2f * (q.y * v.z - q.z * v.y);
            float ty = 2f * (q.z * v.x - q.x * v.z);
            float tz = 2f * (q.x * v.y - q.y * v.x);
            return new Vector3(
                v.x + q.w * tx + (q.y * tz - q.z * ty),
                v.y + q.w * ty + (q.z * tx - q.x * tz),
                v.z + q.w * tz + (q.x * ty - q.y * tx));
        }

        public static float Dot(Quaternion a, Quaternion b) => a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;

        public static Quaternion Inverse(Quaternion q)
        {
            float n = Dot(q, q);
            if (n < kEpsilon) return identity;
            float inv = 1f / n;
            return new Quaternion(-q.x * inv, -q.y * inv, -q.z * inv, q.w * inv);
        }

        public static Quaternion Normalize(Quaternion q)
        {
            float mag = Mathf.Sqrt(Dot(q, q));
            if (mag < kEpsilon) return identity;
            return new Quaternion(q.x / mag, q.y / mag, q.z / mag, q.w / mag);
        }

        public Quaternion normalized => Normalize(this);
        public void Normalize() { this = Normalize(this); }

        public static Quaternion AngleAxis(float angle, Vector3 axis)
        {
            Vector3 n = axis.normalized;
            float half = angle * Mathf.Deg2Rad * 0.5f;
            float s = Mathf.Sin(half);
            return new Quaternion(n.x * s, n.y * s, n.z * s, Mathf.Cos(half));
        }

        // Unity order: extrinsic Z, then X, then Y (== intrinsic Y * X * Z). Degrees.
        public static Quaternion Euler(float x, float y, float z) =>
            AngleAxis(y, Vector3.up) * AngleAxis(x, Vector3.right) * AngleAxis(z, Vector3.forward);

        public static Quaternion Euler(Vector3 euler) => Euler(euler.x, euler.y, euler.z);

        public static Quaternion Slerp(Quaternion a, Quaternion b, float t) => SlerpUnclamped(a, b, Mathf.Clamp01(t));

        public static Quaternion SlerpUnclamped(Quaternion a, Quaternion b, float t)
        {
            float dot = Dot(a, b);
            if (dot < 0f)
            {
                b = new Quaternion(-b.x, -b.y, -b.z, -b.w);
                dot = -dot;
            }
            if (dot > 0.9995f)
            {
                // Nearly parallel: normalized lerp avoids a divide-by-tiny-sin.
                return Normalize(new Quaternion(
                    a.x + (b.x - a.x) * t,
                    a.y + (b.y - a.y) * t,
                    a.z + (b.z - a.z) * t,
                    a.w + (b.w - a.w) * t));
            }
            float theta0 = Mathf.Atan2(Mathf.Sqrt(1f - dot * dot), dot);
            float theta = theta0 * t;
            float sinTheta0 = Mathf.Sin(theta0);
            float s0 = Mathf.Sin(theta0 - theta) / sinTheta0;
            float s1 = Mathf.Sin(theta) / sinTheta0;
            return new Quaternion(
                a.x * s0 + b.x * s1,
                a.y * s0 + b.y * s1,
                a.z * s0 + b.z * s1,
                a.w * s0 + b.w * s1);
        }

        // Unity semantics: operator== treats q and -q as equal (same rotation).
        public static bool operator ==(Quaternion lhs, Quaternion rhs) => Dot(lhs, rhs) > 1f - kEpsilon || Dot(lhs, rhs) < -(1f - kEpsilon);
        public static bool operator !=(Quaternion lhs, Quaternion rhs) => !(lhs == rhs);

        public bool Equals(Quaternion other) => x == other.x && y == other.y && z == other.z && w == other.w;
        public override bool Equals(object other) => other is Quaternion q && Equals(q);
        public override int GetHashCode() => x.GetHashCode() ^ (y.GetHashCode() << 2) ^ (z.GetHashCode() >> 2) ^ (w.GetHashCode() >> 1);
        public override string ToString() => "(" + x.ToString("F5") + ", " + y.ToString("F5") + ", " + z.ToString("F5") + ", " + w.ToString("F5") + ")";
    }
}
