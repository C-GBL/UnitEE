using System;

namespace UnityEngine
{
    public struct Rect : IEquatable<Rect>
    {
        private float m_X;
        private float m_Y;
        private float m_Width;
        private float m_Height;

        public Rect(float x, float y, float width, float height)
        {
            m_X = x; m_Y = y; m_Width = width; m_Height = height;
        }

        public Rect(Vector2 position, Vector2 size)
        {
            m_X = position.x; m_Y = position.y; m_Width = size.x; m_Height = size.y;
        }

        public static Rect zero => new Rect(0f, 0f, 0f, 0f);

        public static Rect MinMaxRect(float xmin, float ymin, float xmax, float ymax) =>
            new Rect(xmin, ymin, xmax - xmin, ymax - ymin);

        public float x { get => m_X; set => m_X = value; }
        public float y { get => m_Y; set => m_Y = value; }
        public float width { get => m_Width; set => m_Width = value; }
        public float height { get => m_Height; set => m_Height = value; }

        public Vector2 position { get => new Vector2(m_X, m_Y); set { m_X = value.x; m_Y = value.y; } }
        public Vector2 size { get => new Vector2(m_Width, m_Height); set { m_Width = value.x; m_Height = value.y; } }
        public Vector2 center
        {
            get => new Vector2(m_X + m_Width * 0.5f, m_Y + m_Height * 0.5f);
            set { m_X = value.x - m_Width * 0.5f; m_Y = value.y - m_Height * 0.5f; }
        }

        public float xMin
        {
            get => m_X;
            set { float oldMax = xMax; m_X = value; m_Width = oldMax - m_X; }
        }

        public float yMin
        {
            get => m_Y;
            set { float oldMax = yMax; m_Y = value; m_Height = oldMax - m_Y; }
        }

        public float xMax { get => m_X + m_Width; set => m_Width = value - m_X; }
        public float yMax { get => m_Y + m_Height; set => m_Height = value - m_Y; }

        public Vector2 min { get => new Vector2(xMin, yMin); set { xMin = value.x; yMin = value.y; } }
        public Vector2 max { get => new Vector2(xMax, yMax); set { xMax = value.x; yMax = value.y; } }

        public bool Contains(Vector2 point) =>
            point.x >= xMin && point.x < xMax && point.y >= yMin && point.y < yMax;

        public bool Contains(Vector3 point) => Contains(new Vector2(point.x, point.y));

        public bool Overlaps(Rect other) =>
            other.xMax > xMin && other.xMin < xMax && other.yMax > yMin && other.yMin < yMax;

        public static bool operator ==(Rect lhs, Rect rhs) => lhs.Equals(rhs);
        public static bool operator !=(Rect lhs, Rect rhs) => !lhs.Equals(rhs);

        public bool Equals(Rect other) =>
            m_X == other.m_X && m_Y == other.m_Y && m_Width == other.m_Width && m_Height == other.m_Height;

        public override bool Equals(object other) => other is Rect r && Equals(r);
        public override int GetHashCode() =>
            m_X.GetHashCode() ^ (m_Width.GetHashCode() << 2) ^ (m_Y.GetHashCode() >> 2) ^ (m_Height.GetHashCode() >> 1);

        public override string ToString() =>
            "(x:" + m_X.ToString("F2") + ", y:" + m_Y.ToString("F2") + ", width:" + m_Width.ToString("F2") + ", height:" + m_Height.ToString("F2") + ")";
    }
}
