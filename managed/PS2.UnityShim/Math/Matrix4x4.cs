using System;

namespace UnityEngine
{
    // Column-major, matching Unity: field mRC is row R, column C; memory order
    // is column 0 (m00,m10,m20,m30), column 1, ... This layout is blittable
    // across the P/Invoke boundary (ADR-002) and matches the VU-friendly
    // column-vector convention used by the native math library.
    public struct Matrix4x4 : IEquatable<Matrix4x4>
    {
        public float m00, m10, m20, m30;
        public float m01, m11, m21, m31;
        public float m02, m12, m22, m32;
        public float m03, m13, m23, m33;

        public static Matrix4x4 zero => default;

        // Unity's linear indexer: index = row + column * 4, matching the
        // column-major field order above.
        public float this[int index]
        {
            get => this[index % 4, index / 4];
            set => this[index % 4, index / 4] = value;
        }

        public float this[int row, int column]
        {
            get
            {
                Vector4 c = GetColumn(column);
                switch (row)
                {
                    case 0: return c.x;
                    case 1: return c.y;
                    case 2: return c.z;
                    case 3: return c.w;
                    default: throw new IndexOutOfRangeException("Invalid matrix row " + row);
                }
            }
            set
            {
                Vector4 c = GetColumn(column);
                switch (row)
                {
                    case 0: c.x = value; break;
                    case 1: c.y = value; break;
                    case 2: c.z = value; break;
                    case 3: c.w = value; break;
                    default: throw new IndexOutOfRangeException("Invalid matrix row " + row);
                }
                SetColumn(column, c);
            }
        }

        public static Matrix4x4 identity
        {
            get
            {
                Matrix4x4 m = default;
                m.m00 = 1f; m.m11 = 1f; m.m22 = 1f; m.m33 = 1f;
                return m;
            }
        }

        public Vector4 GetColumn(int index)
        {
            switch (index)
            {
                case 0: return new Vector4(m00, m10, m20, m30);
                case 1: return new Vector4(m01, m11, m21, m31);
                case 2: return new Vector4(m02, m12, m22, m32);
                case 3: return new Vector4(m03, m13, m23, m33);
                default: throw new IndexOutOfRangeException("Invalid matrix column " + index);
            }
        }

        public void SetColumn(int index, Vector4 column)
        {
            switch (index)
            {
                case 0: m00 = column.x; m10 = column.y; m20 = column.z; m30 = column.w; break;
                case 1: m01 = column.x; m11 = column.y; m21 = column.z; m31 = column.w; break;
                case 2: m02 = column.x; m12 = column.y; m22 = column.z; m32 = column.w; break;
                case 3: m03 = column.x; m13 = column.y; m23 = column.z; m33 = column.w; break;
                default: throw new IndexOutOfRangeException("Invalid matrix column " + index);
            }
        }

        public Vector4 GetRow(int index)
        {
            switch (index)
            {
                case 0: return new Vector4(m00, m01, m02, m03);
                case 1: return new Vector4(m10, m11, m12, m13);
                case 2: return new Vector4(m20, m21, m22, m23);
                case 3: return new Vector4(m30, m31, m32, m33);
                default: throw new IndexOutOfRangeException("Invalid matrix row " + index);
            }
        }

        public void SetRow(int index, Vector4 row)
        {
            switch (index)
            {
                case 0: m00 = row.x; m01 = row.y; m02 = row.z; m03 = row.w; break;
                case 1: m10 = row.x; m11 = row.y; m12 = row.z; m13 = row.w; break;
                case 2: m20 = row.x; m21 = row.y; m22 = row.z; m23 = row.w; break;
                case 3: m30 = row.x; m31 = row.y; m32 = row.z; m33 = row.w; break;
                default: throw new IndexOutOfRangeException("Invalid matrix row " + index);
            }
        }

        public static Matrix4x4 operator *(Matrix4x4 a, Matrix4x4 b)
        {
            Matrix4x4 r = default;
            r.m00 = a.m00 * b.m00 + a.m01 * b.m10 + a.m02 * b.m20 + a.m03 * b.m30;
            r.m01 = a.m00 * b.m01 + a.m01 * b.m11 + a.m02 * b.m21 + a.m03 * b.m31;
            r.m02 = a.m00 * b.m02 + a.m01 * b.m12 + a.m02 * b.m22 + a.m03 * b.m32;
            r.m03 = a.m00 * b.m03 + a.m01 * b.m13 + a.m02 * b.m23 + a.m03 * b.m33;
            r.m10 = a.m10 * b.m00 + a.m11 * b.m10 + a.m12 * b.m20 + a.m13 * b.m30;
            r.m11 = a.m10 * b.m01 + a.m11 * b.m11 + a.m12 * b.m21 + a.m13 * b.m31;
            r.m12 = a.m10 * b.m02 + a.m11 * b.m12 + a.m12 * b.m22 + a.m13 * b.m32;
            r.m13 = a.m10 * b.m03 + a.m11 * b.m13 + a.m12 * b.m23 + a.m13 * b.m33;
            r.m20 = a.m20 * b.m00 + a.m21 * b.m10 + a.m22 * b.m20 + a.m23 * b.m30;
            r.m21 = a.m20 * b.m01 + a.m21 * b.m11 + a.m22 * b.m21 + a.m23 * b.m31;
            r.m22 = a.m20 * b.m02 + a.m21 * b.m12 + a.m22 * b.m22 + a.m23 * b.m32;
            r.m23 = a.m20 * b.m03 + a.m21 * b.m13 + a.m22 * b.m23 + a.m23 * b.m33;
            r.m30 = a.m30 * b.m00 + a.m31 * b.m10 + a.m32 * b.m20 + a.m33 * b.m30;
            r.m31 = a.m30 * b.m01 + a.m31 * b.m11 + a.m32 * b.m21 + a.m33 * b.m31;
            r.m32 = a.m30 * b.m02 + a.m31 * b.m12 + a.m32 * b.m22 + a.m33 * b.m32;
            r.m33 = a.m30 * b.m03 + a.m31 * b.m13 + a.m32 * b.m23 + a.m33 * b.m33;
            return r;
        }

        public static Vector4 operator *(Matrix4x4 m, Vector4 v) => new Vector4(
            m.m00 * v.x + m.m01 * v.y + m.m02 * v.z + m.m03 * v.w,
            m.m10 * v.x + m.m11 * v.y + m.m12 * v.z + m.m13 * v.w,
            m.m20 * v.x + m.m21 * v.y + m.m22 * v.z + m.m23 * v.w,
            m.m30 * v.x + m.m31 * v.y + m.m32 * v.z + m.m33 * v.w);

        // Full 4x4 point transform with perspective divide.
        public Vector3 MultiplyPoint(Vector3 point)
        {
            float px = m00 * point.x + m01 * point.y + m02 * point.z + m03;
            float py = m10 * point.x + m11 * point.y + m12 * point.z + m13;
            float pz = m20 * point.x + m21 * point.y + m22 * point.z + m23;
            float pw = m30 * point.x + m31 * point.y + m32 * point.z + m33;
            float inv = 1f / pw; // NOTE: EE float divide-by-zero saturates, plan 3.1
            return new Vector3(px * inv, py * inv, pz * inv);
        }

        // Affine point transform (no perspective divide) - the fast path.
        public Vector3 MultiplyPoint3x4(Vector3 point) => new Vector3(
            m00 * point.x + m01 * point.y + m02 * point.z + m03,
            m10 * point.x + m11 * point.y + m12 * point.z + m13,
            m20 * point.x + m21 * point.y + m22 * point.z + m23);

        public Vector3 MultiplyVector(Vector3 vector) => new Vector3(
            m00 * vector.x + m01 * vector.y + m02 * vector.z,
            m10 * vector.x + m11 * vector.y + m12 * vector.z,
            m20 * vector.x + m21 * vector.y + m22 * vector.z);

        public Matrix4x4 transpose
        {
            get
            {
                Matrix4x4 r = default;
                r.m00 = m00; r.m01 = m10; r.m02 = m20; r.m03 = m30;
                r.m10 = m01; r.m11 = m11; r.m12 = m21; r.m13 = m31;
                r.m20 = m02; r.m21 = m12; r.m22 = m22; r.m23 = m32;
                r.m30 = m03; r.m31 = m13; r.m32 = m23; r.m33 = m33;
                return r;
            }
        }

        public static Matrix4x4 TRS(Vector3 pos, Quaternion q, Vector3 s)
        {
            float x = q.x, y = q.y, z = q.z, w = q.w;
            Matrix4x4 m = default;
            m.m00 = (1f - 2f * (y * y + z * z)) * s.x;
            m.m10 = (2f * (x * y + z * w)) * s.x;
            m.m20 = (2f * (x * z - y * w)) * s.x;
            m.m01 = (2f * (x * y - z * w)) * s.y;
            m.m11 = (1f - 2f * (x * x + z * z)) * s.y;
            m.m21 = (2f * (y * z + x * w)) * s.y;
            m.m02 = (2f * (x * z + y * w)) * s.z;
            m.m12 = (2f * (y * z - x * w)) * s.z;
            m.m22 = (1f - 2f * (x * x + y * y)) * s.z;
            m.m03 = pos.x;
            m.m13 = pos.y;
            m.m23 = pos.z;
            m.m33 = 1f;
            return m;
        }

        public static Matrix4x4 Rotate(Quaternion q) => TRS(Vector3.zero, q, Vector3.one);

        public static Matrix4x4 Translate(Vector3 vector)
        {
            Matrix4x4 m = identity;
            m.m03 = vector.x; m.m13 = vector.y; m.m23 = vector.z;
            return m;
        }

        public static Matrix4x4 Scale(Vector3 vector)
        {
            Matrix4x4 m = default;
            m.m00 = vector.x; m.m11 = vector.y; m.m22 = vector.z; m.m33 = 1f;
            return m;
        }

        // NOTE: inverse/determinant intentionally omitted for now; the native
        // math library (runtime/src/math, VU0 macro mode) owns the fast path.
        // TODO(spec missing: section 12): decide managed-side fallback.

        public bool Equals(Matrix4x4 other) =>
            m00 == other.m00 && m10 == other.m10 && m20 == other.m20 && m30 == other.m30 &&
            m01 == other.m01 && m11 == other.m11 && m21 == other.m21 && m31 == other.m31 &&
            m02 == other.m02 && m12 == other.m12 && m22 == other.m22 && m32 == other.m32 &&
            m03 == other.m03 && m13 == other.m13 && m23 == other.m23 && m33 == other.m33;

        public override bool Equals(object other) => other is Matrix4x4 m && Equals(m);

        public static bool operator ==(Matrix4x4 lhs, Matrix4x4 rhs) => lhs.Equals(rhs);
        public static bool operator !=(Matrix4x4 lhs, Matrix4x4 rhs) => !lhs.Equals(rhs);

        public override int GetHashCode() =>
            GetColumn(0).GetHashCode() ^ (GetColumn(1).GetHashCode() << 2) ^
            (GetColumn(2).GetHashCode() >> 2) ^ (GetColumn(3).GetHashCode() >> 1);

        public override string ToString() =>
            GetRow(0).ToString() + "\n" + GetRow(1).ToString() + "\n" + GetRow(2).ToString() + "\n" + GetRow(3).ToString() + "\n";
    }
}
