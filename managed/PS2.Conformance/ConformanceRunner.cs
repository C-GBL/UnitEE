using System;
using UnityEngine;
using UnityEngine.Internal;

namespace PS2.Conformance
{
    // D5 conformance suite runner (plan section 2, D5): the same assembly runs
    // in the Unity Editor and on the PS2 target, so there is NO external test
    // framework - plain static methods and golden-value assertions only.
    //
    // Entry point contract: RunAll() returns the number of failed assertions
    // (0 == pass). On target, the boot shim calls RunAll() and the result goes
    // out through the exit code / console log; in the Editor or on a plain
    // host CLR, call RunAll(true) so logging goes to System.Console instead of
    // the (absent) native runtime.
    //
    // TODO(spec missing: section 14): the testing spec defines the full ~400
    // assertion catalogue, the Editor-vs-target comparison harness, and the
    // documented-deviation test format for plan section 7.4 (EE float
    // NaN/Infinity semantics). Only the first two suites exist yet.
    public static class ConformanceRunner
    {
        private static readonly Action[] Suites =
        {
            MathfSuite.Run,
            Vector3Suite.Run,
        };

        public static int RunAll(bool logToConsole)
        {
            ShimConfig.UseConsoleLog = logToConsole;
            return RunAll();
        }

        public static int RunAll()
        {
            ConformanceAssert.Reset();
            for (int i = 0; i < Suites.Length; i++)
            {
                Suites[i]();
            }
            Debug.Log("[conformance] " + ConformanceAssert.Total + " assertions, " + ConformanceAssert.Failed + " failed");
            return ConformanceAssert.Failed;
        }
    }
}
