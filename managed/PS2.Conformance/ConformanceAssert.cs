using UnityEngine;

namespace PS2.Conformance
{
    // Minimal assertion helper for the D5 suite. No test framework: this must
    // AOT-compile through il2cpp and run on the EE with no reflection, no
    // attributes, no exception-driven control flow.
    public static class ConformanceAssert
    {
        public static int Total;
        public static int Failed;

        public static void Reset()
        {
            Total = 0;
            Failed = 0;
        }

        public static void IsTrue(bool condition, string label)
        {
            Total++;
            if (!condition) Fail(label, "expected true");
        }

        public static void AreEqual(float expected, float actual, float tolerance, string label)
        {
            Total++;
            float diff = expected - actual;
            if (diff < 0f) diff = -diff;
            // NOTE: also catches NaN leaking in on host (comparisons with NaN
            // are false, so 'diff <= tolerance' fails and we report).
            if (!(diff <= tolerance))
                Fail(label, "expected " + expected.ToString("R") + " got " + actual.ToString("R"));
        }

        public static void AreEqual(int expected, int actual, string label)
        {
            Total++;
            if (expected != actual)
                Fail(label, "expected " + expected + " got " + actual);
        }

        private static void Fail(string label, string detail)
        {
            Failed++;
            Debug.LogError("[conformance] FAIL " + label + ": " + detail);
        }
    }
}
