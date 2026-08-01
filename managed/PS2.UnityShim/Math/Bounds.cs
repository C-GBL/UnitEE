using System;

namespace UnityEngine
{
    // Axis-aligned bounding box, stored as center + extents like Unity.
    public struct Bounds : IEquatable<Bounds>
    {
        private Vector3 m_Center;
        private Vector3 m_Extents;

        public Bounds(Vector3 center, Vector3 size)
        {
            m_Center = center;
            m_Extents = size * 0.5f;
        }

        public Vector3 center { get => m_Center; set => m_Center = value; }
        public Vector3 extents { get => m_Extents; set => m_Extents = value; }
        public Vector3 size { get => m_Extents * 2f; set => m_Extents = value * 0.5f; }
        public Vector3 min { get => m_Center - m_Extents; set => SetMinMax(value, max); }
        public Vector3 max { get => m_Center + m_Extents; set => SetMinMax(min, value); }

        public void SetMinMax(Vector3 min, Vector3 max)
        {
            m_Extents = (max - min) * 0.5f;
            m_Center = min + m_Extents;
        }

        public void Encapsulate(Vector3 point)
        {
            SetMinMax(Vector3.Min(min, point), Vector3.Max(max, point));
        }

        public void Encapsulate(Bounds bounds)
        {
            Encapsulate(bounds.min);
            Encapsulate(bounds.max);
        }

        public void Expand(float amount)
        {
            amount *= 0.5f;
            m_Extents += new Vector3(amount, amount, amount);
        }

        public bool Contains(Vector3 point)
        {
            Vector3 mn = min, mx = max;
            return point.x >= mn.x && point.x <= mx.x
                && point.y >= mn.y && point.y <= mx.y
                && point.z >= mn.z && point.z <= mx.z;
        }

        public bool Intersects(Bounds bounds)
        {
            Vector3 amn = min, amx = max, bmn = bounds.min, bmx = bounds.max;
            return amn.x <= bmx.x && amx.x >= bmn.x
                && amn.y <= bmx.y && amx.y >= bmn.y
                && amn.z <= bmx.z && amx.z >= bmn.z;
        }

        public static bool operator ==(Bounds lhs, Bounds rhs) => lhs.Equals(rhs);
        public static bool operator !=(Bounds lhs, Bounds rhs) => !lhs.Equals(rhs);

        public bool Equals(Bounds other) => m_Center.Equals(other.m_Center) && m_Extents.Equals(other.m_Extents);
        public override bool Equals(object other) => other is Bounds b && Equals(b);
        public override int GetHashCode() => m_Center.GetHashCode() ^ (m_Extents.GetHashCode() << 2);
        public override string ToString() => "Center: " + m_Center.ToString() + ", Extents: " + m_Extents.ToString();
    }
}
