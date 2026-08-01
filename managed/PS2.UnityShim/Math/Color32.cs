using System;

namespace UnityEngine
{
    // 32-bit RGBA. This is the wire format the exporter and the GS actually
    // use (plan 3.3); keep it exactly 4 bytes and blittable (ADR-002).
    public struct Color32 : IEquatable<Color32>
    {
        public byte r;
        public byte g;
        public byte b;
        public byte a;

        public Color32(byte r, byte g, byte b, byte a) { this.r = r; this.g = g; this.b = b; this.a = a; }

        public static implicit operator Color32(Color c) => new Color32(
            (byte)Mathf.RoundToInt(Mathf.Clamp01(c.r) * 255f),
            (byte)Mathf.RoundToInt(Mathf.Clamp01(c.g) * 255f),
            (byte)Mathf.RoundToInt(Mathf.Clamp01(c.b) * 255f),
            (byte)Mathf.RoundToInt(Mathf.Clamp01(c.a) * 255f));

        public static implicit operator Color(Color32 c) =>
            new Color(c.r / 255f, c.g / 255f, c.b / 255f, c.a / 255f);

        public static Color32 Lerp(Color32 x, Color32 y, float t)
        {
            t = Mathf.Clamp01(t);
            return new Color32(
                (byte)Mathf.RoundToInt(x.r + (y.r - x.r) * t),
                (byte)Mathf.RoundToInt(x.g + (y.g - x.g) * t),
                (byte)Mathf.RoundToInt(x.b + (y.b - x.b) * t),
                (byte)Mathf.RoundToInt(x.a + (y.a - x.a) * t));
        }

        public bool Equals(Color32 other) => r == other.r && g == other.g && b == other.b && a == other.a;
        public override bool Equals(object other) => other is Color32 c && Equals(c);
        public override int GetHashCode() => r | (g << 8) | (b << 16) | (a << 24);
        public override string ToString() => "RGBA(" + r + ", " + g + ", " + b + ", " + a + ")";
    }
}
