using System;

namespace UnityEngine
{
    public struct Vector4 : IEquatable<Vector4>
    {
        public float x;
        public float y;
        public float z;
        public float w;

        public const float kEpsilon = 1e-5f;

        public Vector4(float x, float y, float z, float w) { this.x = x; this.y = y; this.z = z; this.w = w; }
        public Vector4(float x, float y, float z) { this.x = x; this.y = y; this.z = z; this.w = 0f; }

        public static Vector4 zero => new Vector4(0f, 0f, 0f, 0f);
        public static Vector4 one => new Vector4(1f, 1f, 1f, 1f);

        public float magnitude => Mathf.Sqrt(x * x + y * y + z * z + w * w);
        public float sqrMagnitude => x * x + y * y + z * z + w * w;

        public Vector4 normalized
        {
            get
            {
                float mag = magnitude;
                return mag > kEpsilon ? this / mag : zero;
            }
        }

        public void Normalize() { this = normalized; }

        public static float Dot(Vector4 a, Vector4 b) => a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        public static float Distance(Vector4 a, Vector4 b) => (a - b).magnitude;

        public static Vector4 Lerp(Vector4 a, Vector4 b, float t)
        {
            t = Mathf.Clamp01(t);
            return new Vector4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
        }

        public static Vector4 operator +(Vector4 a, Vector4 b) => new Vector4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w);
        public static Vector4 operator -(Vector4 a, Vector4 b) => new Vector4(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w);
        public static Vector4 operator -(Vector4 a) => new Vector4(-a.x, -a.y, -a.z, -a.w);
        public static Vector4 operator *(Vector4 a, float d) => new Vector4(a.x * d, a.y * d, a.z * d, a.w * d);
        public static Vector4 operator *(float d, Vector4 a) => new Vector4(a.x * d, a.y * d, a.z * d, a.w * d);
        public static Vector4 operator /(Vector4 a, float d) => new Vector4(a.x / d, a.y / d, a.z / d, a.w / d);

        public static bool operator ==(Vector4 lhs, Vector4 rhs) => (lhs - rhs).sqrMagnitude < kEpsilon * kEpsilon;
        public static bool operator !=(Vector4 lhs, Vector4 rhs) => !(lhs == rhs);

        // Unity semantics: Vector3 <-> Vector4 conversions drop/zero w.
        public static implicit operator Vector4(Vector3 v) => new Vector4(v.x, v.y, v.z, 0f);
        public static implicit operator Vector3(Vector4 v) => new Vector3(v.x, v.y, v.z);

        public bool Equals(Vector4 other) => x == other.x && y == other.y && z == other.z && w == other.w;
        public override bool Equals(object other) => other is Vector4 v && Equals(v);
        public override int GetHashCode() => x.GetHashCode() ^ (y.GetHashCode() << 2) ^ (z.GetHashCode() >> 2) ^ (w.GetHashCode() >> 1);
        public override string ToString() => "(" + x.ToString("F2") + ", " + y.ToString("F2") + ", " + z.ToString("F2") + ", " + w.ToString("F2") + ")";
    }
}
