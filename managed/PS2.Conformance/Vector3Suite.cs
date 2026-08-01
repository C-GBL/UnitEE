using UnityEngine;

namespace PS2.Conformance
{
    // Golden-value tests for UnityEngine.Vector3.
    // See MathfSuite header for the plan section 7.4 NaN/Infinity policy.
    public static class Vector3Suite
    {
        private const float Tol = 1e-5f;

        public static void Run()
        {
            // normalize
            Vector3 n = new Vector3(3f, 4f, 0f).normalized;
            ConformanceAssert.AreEqual(0.6f, n.x, Tol, "Vector3.normalized x");
            ConformanceAssert.AreEqual(0.8f, n.y, Tol, "Vector3.normalized y");
            ConformanceAssert.AreEqual(0f, n.z, Tol, "Vector3.normalized z");
            ConformanceAssert.AreEqual(1f, n.magnitude, Tol, "Vector3.normalized magnitude");
            ConformanceAssert.IsTrue(Vector3.zero.normalized == Vector3.zero, "Vector3 zero.normalized is zero (Unity semantics)");

            // magnitude
            ConformanceAssert.AreEqual(3f, new Vector3(1f, 2f, 2f).magnitude, Tol, "Vector3.magnitude(1,2,2)");
            ConformanceAssert.AreEqual(9f, new Vector3(1f, 2f, 2f).sqrMagnitude, Tol, "Vector3.sqrMagnitude(1,2,2)");

            // dot
            ConformanceAssert.AreEqual(12f, Vector3.Dot(new Vector3(1f, 2f, 3f), new Vector3(4f, -5f, 6f)), Tol, "Vector3.Dot golden");
            ConformanceAssert.AreEqual(0f, Vector3.Dot(Vector3.up, Vector3.forward), Tol, "Vector3.Dot orthogonal");
            ConformanceAssert.AreEqual(1f, Vector3.Dot(Vector3.up, Vector3.up), Tol, "Vector3.Dot parallel");

            // cross
            ConformanceAssert.IsTrue(Vector3.Cross(Vector3.right, Vector3.up) == Vector3.forward, "Vector3.Cross(right,up) == forward (left-handed)");
            Vector3 c = Vector3.Cross(new Vector3(2f, 3f, 4f), new Vector3(5f, 6f, 7f));
            ConformanceAssert.AreEqual(-3f, c.x, Tol, "Vector3.Cross golden x");
            ConformanceAssert.AreEqual(6f, c.y, Tol, "Vector3.Cross golden y");
            ConformanceAssert.AreEqual(-3f, c.z, Tol, "Vector3.Cross golden z");
            ConformanceAssert.AreEqual(0f, Vector3.Dot(c, new Vector3(2f, 3f, 4f)), Tol, "Vector3.Cross perpendicular to a");

            // distance
            ConformanceAssert.AreEqual(5f, Vector3.Distance(new Vector3(1f, 1f, 1f), new Vector3(4f, 5f, 1f)), Tol, "Vector3.Distance 3-4-5");

            // lerp
            Vector3 l = Vector3.Lerp(Vector3.zero, new Vector3(10f, -10f, 4f), 0.5f);
            ConformanceAssert.AreEqual(5f, l.x, Tol, "Vector3.Lerp x");
            ConformanceAssert.AreEqual(-5f, l.y, Tol, "Vector3.Lerp y");
            ConformanceAssert.AreEqual(2f, l.z, Tol, "Vector3.Lerp z");
            ConformanceAssert.IsTrue(Vector3.Lerp(Vector3.zero, Vector3.one, 2f) == Vector3.one, "Vector3.Lerp clamps");

            // operators / scale
            ConformanceAssert.IsTrue(new Vector3(1f, 2f, 3f) + new Vector3(4f, 5f, 6f) == new Vector3(5f, 7f, 9f), "Vector3 operator+");
            ConformanceAssert.IsTrue(Vector3.Scale(new Vector3(1f, 2f, 3f), new Vector3(2f, 3f, 4f)) == new Vector3(2f, 6f, 12f), "Vector3.Scale");

            // MoveTowards
            Vector3 m = Vector3.MoveTowards(Vector3.zero, new Vector3(0f, 0f, 10f), 4f);
            ConformanceAssert.AreEqual(4f, m.z, Tol, "Vector3.MoveTowards step");
            ConformanceAssert.IsTrue(Vector3.MoveTowards(Vector3.zero, Vector3.one, 10f) == Vector3.one, "Vector3.MoveTowards overshoot");
        }
    }
}
