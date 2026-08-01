using System;

namespace UnityEngine
{
    // Unity-compatible float math (plan section 7.1 "Core types").
    // Everything is float: per plan section 3.1 doubles are poison on the EE
    // (every double op becomes a libgcc soft-float call), so no double
    // overloads are provided. Implemented over System.MathF, which exists on
    // netstandard2.1 and AOT-compiles to plain C library calls under IL2CPP.
    public static class Mathf
    {
        public const float PI = 3.14159274f;
        public const float Deg2Rad = PI / 180f;
        public const float Rad2Deg = 180f / PI;

        // API-compat constants. Plan sections 3.1 and 7.4: the EE FPU has no
        // NaN and no Infinity; hardware float ops saturate. Code holding these
        // values behaves differently on target than in the Editor.
        public const float Infinity = float.PositiveInfinity;
        public const float NegativeInfinity = float.NegativeInfinity;

        // Smallest positive float. On the EE denormals flush to zero (plan 3.1).
        public static readonly float Epsilon = float.Epsilon;

        public static float Abs(float f) => MathF.Abs(f);
        public static int Abs(int value) => value < 0 ? -value : value;

        public static float Min(float a, float b) => a < b ? a : b;
        public static int Min(int a, int b) => a < b ? a : b;
        public static float Max(float a, float b) => a > b ? a : b;
        public static int Max(int a, int b) => a > b ? a : b;

        public static float Clamp(float value, float min, float max) => value < min ? min : (value > max ? max : value);
        public static int Clamp(int value, int min, int max) => value < min ? min : (value > max ? max : value);
        public static float Clamp01(float value) => value < 0f ? 0f : (value > 1f ? 1f : value);

        public static float Lerp(float a, float b, float t) => a + (b - a) * Clamp01(t);
        public static float LerpUnclamped(float a, float b, float t) => a + (b - a) * t;

        public static float MoveTowards(float current, float target, float maxDelta)
        {
            if (Abs(target - current) <= maxDelta) return target;
            return current + Sign(target - current) * maxDelta;
        }

        public static float Sqrt(float f) => MathF.Sqrt(f);
        public static float Sin(float f) => MathF.Sin(f);
        public static float Cos(float f) => MathF.Cos(f);
        public static float Tan(float f) => MathF.Tan(f);
        public static float Atan2(float y, float x) => MathF.Atan2(y, x);
        public static float Pow(float f, float p) => MathF.Pow(f, p);
        public static float Exp(float power) => MathF.Exp(power);
        public static float Log(float f) => MathF.Log(f);
        public static float Log(float f, float p) => MathF.Log(f, p);

        public static float Floor(float f) => MathF.Floor(f);
        public static float Ceil(float f) => MathF.Ceiling(f);
        // Banker's rounding (round-half-to-even), same as Unity's Mathf.Round.
        public static float Round(float f) => MathF.Round(f);

        public static int FloorToInt(float f) => (int)MathF.Floor(f);
        public static int CeilToInt(float f) => (int)MathF.Ceiling(f);
        public static int RoundToInt(float f) => (int)MathF.Round(f);

        // Unity semantics: Sign(0) == 1.
        public static float Sign(float f) => f >= 0f ? 1f : -1f;

        public static float Repeat(float t, float length) => Clamp(t - Floor(t / length) * length, 0f, length);

        public static float PingPong(float t, float length)
        {
            t = Repeat(t, length * 2f);
            return length - Abs(t - length);
        }

        public static float InverseLerp(float a, float b, float value) => a != b ? Clamp01((value - a) / (b - a)) : 0f;

        public static bool Approximately(float a, float b) =>
            Abs(b - a) < Max(1e-6f * Max(Abs(a), Abs(b)), Epsilon * 8f);

        public static float SmoothStep(float from, float to, float t)
        {
            t = Clamp01(t);
            t = -2f * t * t * t + 3f * t * t;
            return to * t + from * (1f - t);
        }
    }
}
