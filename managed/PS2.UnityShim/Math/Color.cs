using System;

namespace UnityEngine
{
    public struct Color : IEquatable<Color>
    {
        public float r;
        public float g;
        public float b;
        public float a;

        public Color(float r, float g, float b, float a) { this.r = r; this.g = g; this.b = b; this.a = a; }
        public Color(float r, float g, float b) { this.r = r; this.g = g; this.b = b; this.a = 1f; }

        public static Color red => new Color(1f, 0f, 0f, 1f);
        public static Color green => new Color(0f, 1f, 0f, 1f);
        public static Color blue => new Color(0f, 0f, 1f, 1f);
        public static Color white => new Color(1f, 1f, 1f, 1f);
        public static Color black => new Color(0f, 0f, 0f, 1f);
        public static Color yellow => new Color(1f, 235f / 255f, 4f / 255f, 1f);
        public static Color cyan => new Color(0f, 1f, 1f, 1f);
        public static Color magenta => new Color(1f, 0f, 1f, 1f);
        public static Color gray => new Color(0.5f, 0.5f, 0.5f, 1f);
        public static Color grey => gray;
        public static Color clear => new Color(0f, 0f, 0f, 0f);

        public static Color operator +(Color x, Color y) => new Color(x.r + y.r, x.g + y.g, x.b + y.b, x.a + y.a);
        public static Color operator -(Color x, Color y) => new Color(x.r - y.r, x.g - y.g, x.b - y.b, x.a - y.a);
        public static Color operator *(Color x, Color y) => new Color(x.r * y.r, x.g * y.g, x.b * y.b, x.a * y.a);
        public static Color operator *(Color x, float d) => new Color(x.r * d, x.g * d, x.b * d, x.a * d);
        public static Color operator *(float d, Color x) => new Color(x.r * d, x.g * d, x.b * d, x.a * d);
        public static Color operator /(Color x, float d) => new Color(x.r / d, x.g / d, x.b / d, x.a / d);

        public static Color Lerp(Color x, Color y, float t)
        {
            t = Mathf.Clamp01(t);
            return new Color(
                x.r + (y.r - x.r) * t,
                x.g + (y.g - x.g) * t,
                x.b + (y.b - x.b) * t,
                x.a + (y.a - x.a) * t);
        }

        public static Color LerpUnclamped(Color x, Color y, float t) => new Color(
            x.r + (y.r - x.r) * t,
            x.g + (y.g - x.g) * t,
            x.b + (y.b - x.b) * t,
            x.a + (y.a - x.a) * t);

        // Unity semantics: approximate equality via the Vector4 comparison.
        public static bool operator ==(Color lhs, Color rhs)
        {
            float dr = lhs.r - rhs.r, dg = lhs.g - rhs.g, db = lhs.b - rhs.b, da = lhs.a - rhs.a;
            return dr * dr + dg * dg + db * db + da * da < 9.99999944e-11f;
        }

        public static bool operator !=(Color lhs, Color rhs) => !(lhs == rhs);

        public bool Equals(Color other) => r == other.r && g == other.g && b == other.b && a == other.a;
        public override bool Equals(object other) => other is Color c && Equals(c);
        public override int GetHashCode() => r.GetHashCode() ^ (g.GetHashCode() << 2) ^ (b.GetHashCode() >> 2) ^ (a.GetHashCode() >> 1);
        public override string ToString() => "RGBA(" + r.ToString("F3") + ", " + g.ToString("F3") + ", " + b.ToString("F3") + ", " + a.ToString("F3") + ")";
    }
}
