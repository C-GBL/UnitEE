using UnityEngine;

namespace PS2.Conformance
{
    // Golden-value tests for UnityEngine.Mathf.
    //
    // IMPORTANT (plan section 7.4): float NaN/Infinity semantics DIFFER on the
    // PS2 EE FPU - no NaN, no Infinity, saturating overflow, flush-to-zero
    // denormals, round-toward-zero. Assertions involving NaN/Inf/denormals
    // MUST NOT be added here as plain golden values; they will be authored
    // later as documented-deviation tests with per-platform expectations.
    // TODO(spec missing: section 14): deviation-test format.
    public static class MathfSuite
    {
        private const float Tol = 1e-5f;

        public static void Run()
        {
            // Sqrt
            ConformanceAssert.AreEqual(1.4142135f, Mathf.Sqrt(2f), Tol, "Mathf.Sqrt(2)");
            ConformanceAssert.AreEqual(3f, Mathf.Sqrt(9f), Tol, "Mathf.Sqrt(9)");
            ConformanceAssert.AreEqual(0f, Mathf.Sqrt(0f), Tol, "Mathf.Sqrt(0)");

            // Trig at known angles
            ConformanceAssert.AreEqual(0.5f, Mathf.Sin(Mathf.PI / 6f), Tol, "Mathf.Sin(pi/6)");
            ConformanceAssert.AreEqual(1f, Mathf.Sin(Mathf.PI / 2f), Tol, "Mathf.Sin(pi/2)");
            ConformanceAssert.AreEqual(0.5f, Mathf.Cos(Mathf.PI / 3f), Tol, "Mathf.Cos(pi/3)");
            ConformanceAssert.AreEqual(1f, Mathf.Cos(0f), Tol, "Mathf.Cos(0)");
            ConformanceAssert.AreEqual(1f, Mathf.Tan(Mathf.PI / 4f), Tol, "Mathf.Tan(pi/4)");
            ConformanceAssert.AreEqual(Mathf.PI / 4f, Mathf.Atan2(1f, 1f), Tol, "Mathf.Atan2(1,1)");

            // Exp / Pow / Log
            ConformanceAssert.AreEqual(1024f, Mathf.Pow(2f, 10f), 1e-2f, "Mathf.Pow(2,10)");
            ConformanceAssert.AreEqual(2.7182817f, Mathf.Exp(1f), Tol, "Mathf.Exp(1)");
            ConformanceAssert.AreEqual(1f, Mathf.Log(Mathf.Exp(1f)), Tol, "Mathf.Log(e)");
            ConformanceAssert.AreEqual(3f, Mathf.Log(8f, 2f), Tol, "Mathf.Log(8,2)");

            // Rounding family (Round is round-half-to-even, like Unity)
            ConformanceAssert.AreEqual(1f, Mathf.Floor(1.7f), Tol, "Mathf.Floor(1.7)");
            ConformanceAssert.AreEqual(2f, Mathf.Ceil(1.2f), Tol, "Mathf.Ceil(1.2)");
            ConformanceAssert.AreEqual(2f, Mathf.Round(2.5f), Tol, "Mathf.Round(2.5)");
            ConformanceAssert.AreEqual(4f, Mathf.Round(3.5f), Tol, "Mathf.Round(3.5)");
            ConformanceAssert.AreEqual(-2, Mathf.FloorToInt(-1.2f), "Mathf.FloorToInt(-1.2)");
            ConformanceAssert.AreEqual(1, Mathf.FloorToInt(1.7f), "Mathf.FloorToInt(1.7)");
            ConformanceAssert.AreEqual(-1, Mathf.CeilToInt(-1.2f), "Mathf.CeilToInt(-1.2)");
            ConformanceAssert.AreEqual(2, Mathf.CeilToInt(1.2f), "Mathf.CeilToInt(1.2)");
            ConformanceAssert.AreEqual(0, Mathf.RoundToInt(0.5f), "Mathf.RoundToInt(0.5)");
            ConformanceAssert.AreEqual(2, Mathf.RoundToInt(1.5f), "Mathf.RoundToInt(1.5)");

            // Abs / Min / Max / Sign
            ConformanceAssert.AreEqual(2.5f, Mathf.Abs(-2.5f), Tol, "Mathf.Abs(-2.5)");
            ConformanceAssert.AreEqual(7, Mathf.Abs(-7), "Mathf.Abs(-7)");
            ConformanceAssert.AreEqual(3f, Mathf.Min(3f, 5f), Tol, "Mathf.Min(3,5)");
            ConformanceAssert.AreEqual(5f, Mathf.Max(3f, 5f), Tol, "Mathf.Max(3,5)");
            ConformanceAssert.AreEqual(-4, Mathf.Min(-4, 4), "Mathf.Min(-4,4) int");
            ConformanceAssert.AreEqual(4, Mathf.Max(-4, 4), "Mathf.Max(-4,4) int");
            ConformanceAssert.AreEqual(-1f, Mathf.Sign(-3f), Tol, "Mathf.Sign(-3)");
            ConformanceAssert.AreEqual(1f, Mathf.Sign(0f), Tol, "Mathf.Sign(0) == 1 (Unity semantics)");
            ConformanceAssert.AreEqual(1f, Mathf.Sign(2.5f), Tol, "Mathf.Sign(2.5)");

            // Clamp edge cases
            ConformanceAssert.AreEqual(3f, Mathf.Clamp(5f, 0f, 3f), Tol, "Mathf.Clamp above");
            ConformanceAssert.AreEqual(0f, Mathf.Clamp(-1f, 0f, 3f), Tol, "Mathf.Clamp below");
            ConformanceAssert.AreEqual(2f, Mathf.Clamp(2f, 0f, 3f), Tol, "Mathf.Clamp inside");
            ConformanceAssert.AreEqual(3, Mathf.Clamp(5, 0, 3), "Mathf.Clamp int above");
            ConformanceAssert.AreEqual(0f, Mathf.Clamp01(-0.5f), Tol, "Mathf.Clamp01 below");
            ConformanceAssert.AreEqual(1f, Mathf.Clamp01(1.5f), Tol, "Mathf.Clamp01 above");
            ConformanceAssert.AreEqual(0.25f, Mathf.Clamp01(0.25f), Tol, "Mathf.Clamp01 inside");

            // Lerp family
            ConformanceAssert.AreEqual(2.5f, Mathf.Lerp(0f, 10f, 0.25f), Tol, "Mathf.Lerp mid");
            ConformanceAssert.AreEqual(10f, Mathf.Lerp(0f, 10f, 2f), Tol, "Mathf.Lerp clamps above");
            ConformanceAssert.AreEqual(0f, Mathf.Lerp(0f, 10f, -1f), Tol, "Mathf.Lerp clamps below");
            ConformanceAssert.AreEqual(15f, Mathf.LerpUnclamped(0f, 10f, 1.5f), Tol, "Mathf.LerpUnclamped above");
            ConformanceAssert.AreEqual(-5f, Mathf.LerpUnclamped(0f, 10f, -0.5f), Tol, "Mathf.LerpUnclamped below");

            // MoveTowards
            ConformanceAssert.AreEqual(1f, Mathf.MoveTowards(0f, 5f, 1f), Tol, "Mathf.MoveTowards step");
            ConformanceAssert.AreEqual(5f, Mathf.MoveTowards(4f, 5f, 3f), Tol, "Mathf.MoveTowards overshoot");
            ConformanceAssert.AreEqual(4.75f, Mathf.MoveTowards(5f, 4f, 0.25f), Tol, "Mathf.MoveTowards down");

            // Repeat / PingPong edge cases
            ConformanceAssert.AreEqual(1.5f, Mathf.Repeat(5.5f, 2f), Tol, "Mathf.Repeat(5.5,2)");
            ConformanceAssert.AreEqual(0.75f, Mathf.Repeat(-0.25f, 1f), Tol, "Mathf.Repeat negative");
            ConformanceAssert.AreEqual(0f, Mathf.Repeat(3f, 3f), Tol, "Mathf.Repeat at length");
            ConformanceAssert.AreEqual(0.5f, Mathf.PingPong(3.5f, 1f), Tol, "Mathf.PingPong(3.5,1)");
            ConformanceAssert.AreEqual(0.75f, Mathf.PingPong(1.25f, 1f), Tol, "Mathf.PingPong(1.25,1)");
            ConformanceAssert.AreEqual(0.25f, Mathf.PingPong(0.25f, 1f), Tol, "Mathf.PingPong(0.25,1)");

            // InverseLerp
            ConformanceAssert.AreEqual(0.25f, Mathf.InverseLerp(0f, 10f, 2.5f), Tol, "Mathf.InverseLerp");
            ConformanceAssert.AreEqual(0.75f, Mathf.InverseLerp(10f, 0f, 2.5f), Tol, "Mathf.InverseLerp reversed");
            ConformanceAssert.AreEqual(0f, Mathf.InverseLerp(5f, 5f, 7f), Tol, "Mathf.InverseLerp degenerate");
            ConformanceAssert.AreEqual(1f, Mathf.InverseLerp(0f, 10f, 25f), Tol, "Mathf.InverseLerp clamps");

            // SmoothStep
            ConformanceAssert.AreEqual(0.15625f, Mathf.SmoothStep(0f, 1f, 0.25f), Tol, "Mathf.SmoothStep(0.25)");
            ConformanceAssert.AreEqual(0.5f, Mathf.SmoothStep(0f, 1f, 0.5f), Tol, "Mathf.SmoothStep(0.5)");
            ConformanceAssert.AreEqual(10f, Mathf.SmoothStep(10f, 20f, 0f), Tol, "Mathf.SmoothStep at 0");
            ConformanceAssert.AreEqual(20f, Mathf.SmoothStep(10f, 20f, 1f), Tol, "Mathf.SmoothStep at 1");

            // Approximately
            ConformanceAssert.IsTrue(Mathf.Approximately(1f, 1f + 1e-7f), "Mathf.Approximately near");
            ConformanceAssert.IsTrue(!Mathf.Approximately(1f, 1.001f), "Mathf.Approximately far");

            // Constants
            ConformanceAssert.AreEqual(180f, Mathf.PI * Mathf.Rad2Deg, 1e-3f, "Rad2Deg roundtrip");
            ConformanceAssert.AreEqual(Mathf.PI, 180f * Mathf.Deg2Rad, Tol, "Deg2Rad roundtrip");
        }
    }
}
