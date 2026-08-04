#!/usr/bin/env python3
"""Managed half of the M6 gate build (plan section 9, M6).

Pipeline:  tests/HelloEE.cs --csc--> managed/  --UnityLinker--> stripped/
           --il2cpp.exe--> gen/ (C++ + Data/{Metadata,Resources})

Everything lands under build/m6-managed/. The EE half is il2cpp-port/
CMakeLists.txt, pointed at gen/ via -DM6_GENERATED.

Paths below are the verified environment (docs/development.md; re-check on any
Unity upgrade). Run:  python il2cpp-port/build_m6.py
"""
import os
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UNITY = "C:/Program Files/Unity/Hub/Editor/6000.0.47f1/Editor/Data"
DEPLOY = UNITY + "/il2cpp/build/deploy"
AOT_BCL = UNITY + "/MonoBleedingEdge/lib/mono/unityaot-win32"
CSC = "C:/Program Files/dotnet/sdk/9.0.304/Roslyn/bincore/csc.dll"

# --synthetic: the plan's 5,000-line synthetic script project (M6 task 7),
# generated on the fly. Same entry-point shape (class HelloEE, static int
# Main) so main_ee.cpp and the CMake -O2 user-assembly split apply as-is.
SYNTHETIC = "--synthetic" in sys.argv

WORK = os.path.join(REPO, "build", "m6-managed" + ("-synth" if SYNTHETIC else ""))
MANAGED = os.path.join(WORK, "managed")
STRIPPED = os.path.join(WORK, "stripped")
GEN = os.path.join(WORK, "gen")

SYNTH_CLASSES = 105  # ~5,060 generated lines; the plan asks for 5,000


def write_synthetic(path):
    """~5,000 lines of representative game-script C#: value types, enums,
    interfaces, inheritance, generics, collections, switches, string work.
    Every class is reached from Main so aggressive stripping keeps it and
    the measurement is honest."""
    lines = []
    w = lines.append
    w("using System;")
    w("using System.Collections.Generic;")
    w("")
    w("public interface IStep { int Step(int input); }")
    w("")
    w("public abstract class ActorBase")
    w("{")
    w("    public int Health;")
    w("    public abstract int Tick(int dt);")
    w("    public virtual string Describe() { return GetType().Name; }")
    w("}")
    w("")
    for i in range(SYNTH_CLASSES):
        w("public enum Mode%d { Idle, Move, Attack, Flee }" % i)
        w("")
        w("public struct Vec%d" % i)
        w("{")
        w("    public float X, Y;")
        w("    public Vec%d(float x, float y) { X = x; Y = y; }" % i)
        w("    public float Dot(Vec%d o) { return X * o.X + Y * o.Y; }" % i)
        w("}")
        w("")
        w("public class Actor%d : ActorBase, IStep" % i)
        w("{")
        w("    private readonly List<int> m_history = new List<int>();")
        w("    private Mode%d m_mode = Mode%d.Idle;" % (i, i))
        w("    private Vec%d m_pos = new Vec%d(%d.5f, %d.25f);" % (i, i, i, i + 1))
        w("    public int Score { get; private set; }")
        w("")
        w("    public Actor%d() { Health = %d; }" % (i, 50 + i))
        w("")
        w("    public int Step(int input)")
        w("    {")
        w("        m_history.Add(input);")
        w("        switch (m_mode)")
        w("        {")
        w("            case Mode%d.Idle: m_mode = Mode%d.Move; return input + 1;" % (i, i))
        w("            case Mode%d.Move: m_mode = Mode%d.Attack; return input * 2;" % (i, i))
        w("            case Mode%d.Attack: m_mode = Mode%d.Flee; return input - 3;" % (i, i))
        w("            default: m_mode = Mode%d.Idle; return input;" % i)
        w("        }")
        w("    }")
        w("")
        w("    public override int Tick(int dt)")
        w("    {")
        w("        int acc = Health;")
        w("        for (int n = 0; n < 4; n++)")
        w("            acc = Step(acc + dt);")
        w("        Score = acc + (int)m_pos.Dot(new Vec%d(1f, 2f));" % i)
        w("        foreach (int h in m_history)")
        w("            acc += h & 15;")
        w("        return acc;")
        w("    }")
        w("")
        w("    public override string Describe()")
        w("    {")
        w("        return string.Format(\"actor%d mode={0} score={1}\", m_mode, Score);" % i)
        w("    }")
        w("}")
        w("")
    w("public static class HelloEE")
    w("{")
    w("    public static int Main()")
    w("    {")
    w("        var actors = new List<ActorBase>();")
    for i in range(SYNTH_CLASSES):
        w("        actors.Add(new Actor%d());" % i)
    w("        long checksum = 0;")
    w("        foreach (ActorBase actor in actors)")
    w("            checksum += actor.Tick(7) + actor.Describe().Length;")
    w("        Console.WriteLine(string.Format(\"M6_SYNTH_CHECKSUM={0}\", checksum));")
    w("        Console.WriteLine(checksum != 0 ? \"M6_GATE_FEATURES_OK\" : \"M6_GATE_FEATURES_FAIL\");")
    w("        return checksum != 0 ? 0 : 1;")
    w("    }")
    w("}")
    text = "\n".join(lines) + "\n"
    with open(path, "w", newline="\n") as f:
        f.write(text)
    print("[build_m6] synthetic source: %d lines" % (text.count("\n")))


def run(argv, what):
    print("[build_m6] " + what)
    result = subprocess.run(argv)
    if result.returncode != 0:
        sys.exit("[build_m6] FAILED (%d): %s" % (result.returncode, what))


def main():
    if SYNTHETIC:
        os.makedirs(WORK, exist_ok=True)
        source = os.path.join(WORK, "Synth.cs")
        write_synthetic(source)
    else:
        source = os.path.join(REPO, "il2cpp-port", "tests", "HelloEE.cs")

    # 1. Compile against the AOT BCL. One flat directory holding the test
    #    assembly plus the whole profile: UnityLinker's --search-directory
    #    cannot take a comma list when paths contain spaces (verify-log).
    if os.path.isdir(MANAGED):
        shutil.rmtree(MANAGED)
    os.makedirs(MANAGED)
    for name in os.listdir(AOT_BCL):
        if name.endswith(".dll"):
            shutil.copy2(os.path.join(AOT_BCL, name), MANAGED)
    run([
        "dotnet", "exec", CSC,
        "-nostdlib", "-noconfig", "-deterministic+", "-optimize+",
        "-target:library",
        "-r:" + os.path.join(MANAGED, "mscorlib.dll"),
        "-out:" + os.path.join(MANAGED, "HelloEE.dll"),
        source,
    ], "csc HelloEE.cs")

    # 2. Strip. Aggressive is the profile a real game export will use; the
    #    gate must measure the same configuration.
    if os.path.isdir(STRIPPED):
        shutil.rmtree(STRIPPED)
    run([
        DEPLOY + "/UnityLinker.exe",
        "--include-assembly=" + os.path.join(MANAGED, "HelloEE.dll"),
        "--search-directory=" + MANAGED,
        "--out=" + STRIPPED,
        "--core-action=link",
        "--i18n=none",
        "--rule-set=aggressive",
    ], "UnityLinker strip")

    # 3. AOT-convert to C++.
    if os.path.isdir(GEN):
        shutil.rmtree(GEN)
    run([
        DEPLOY + "/il2cpp.exe",
        "--convert-to-cpp",
        "--directory=" + STRIPPED,
        "--generatedcppdir=" + GEN,
        "--dotnetprofile=unityaot-win32",
        # Plan M6 task 6, measured result: on this il2cpp version the release
        # configuration below changes NOTHING for this profile -- null checks
        # are already off in standalone conversion, and the 1063 bounds-check
        # sites all sit in shared-generic BCL code that ignores the switch
        # (site counts identical with and without). Kept because they are the
        # documented release configuration and cost nothing; the real size
        # levers are -Os and strip in the EE build (see m6-report.md).
        "--emit-null-checks=false",
        "--enable-array-bounds-check=false",
        "--enable-divide-by-zero-check=false",
    ], "il2cpp convert-to-cpp")

    print("[build_m6] OK: generated sources in " + GEN)
    print("[build_m6] next: cmake -G Ninja -S il2cpp-port -B build/m6-ee "
          "-DCMAKE_TOOLCHAIN_FILE=tools/cmake/ps2-toolchain.cmake "
          "-DM6_GENERATED=" + GEN)


if __name__ == "__main__":
    main()
