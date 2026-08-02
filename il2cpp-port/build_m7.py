#!/usr/bin/env python3
"""Managed half of the M7 build (plan section 9, M7).

Pipeline:
  managed/PS2.UnityShim/**/*.cs --csc--> PS2.UnityShim.dll
  <unity project>/Assets/PS2Scripts/*.cs --csc--> Assembly-CSharp.dll
       (same assembly NAME Unity gives loose scripts, so the type names the
        exporter wrote into the .p2b resolve on target)
  UnityLinker (+link.xml preserving both: Type.GetType + lifecycle
       reflection require it) -> stripped/ -> il2cpp convert -> gen/

The EE half: il2cpp-port/CMakeLists.txt with M6_MAIN=main_m7.cpp and
PS2UR_RUNTIME pointing at build/ps2-release/runtime (build that first:
cmake --preset ps2-release && cmake --build --preset ps2-release).

Run:  python il2cpp-port/build_m7.py
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
UNITY_PROJECT = "C:/Users/Ash/Unity2PS2-UnityProject"

WORK = os.path.join(REPO, "build", "m7-managed")
MANAGED = os.path.join(WORK, "managed")
STRIPPED = os.path.join(WORK, "stripped")
GEN = os.path.join(WORK, "gen")

LINK_XML = """<linker>
  <!-- The game assembly is reached via Type.GetType and its lifecycle
       methods via reflection (UnityEngine.Internal.Runtime): nothing may be
       stripped from it. The shim hosts the dispatcher itself. -->
  <assembly fullname="Assembly-CSharp" preserve="all"/>
  <assembly fullname="PS2.UnityShim" preserve="all"/>
</linker>
"""


def run(argv, what):
    print("[build_m7] " + what)
    result = subprocess.run(argv)
    if result.returncode != 0:
        sys.exit("[build_m7] FAILED (%d): %s" % (result.returncode, what))


def shim_sources():
    sources = []
    for root, dirs, files in os.walk(os.path.join(REPO, "managed", "PS2.UnityShim")):
        dirs[:] = [d for d in dirs if d not in ("obj", "bin")]
        for name in files:
            if name.endswith(".cs"):
                sources.append(os.path.join(root, name))
    return sorted(sources)


def game_sources():
    folder = os.path.join(UNITY_PROJECT, "Assets", "PS2Scripts")
    return sorted(os.path.join(folder, f) for f in os.listdir(folder)
                  if f.endswith(".cs"))


def main():
    if os.path.isdir(MANAGED):
        shutil.rmtree(MANAGED)
    os.makedirs(MANAGED)
    for name in os.listdir(AOT_BCL):
        if name.endswith(".dll"):
            shutil.copy2(os.path.join(AOT_BCL, name), MANAGED)

    run(["dotnet", "exec", CSC,
         "-nostdlib", "-noconfig", "-deterministic+", "-optimize+",
         "-target:library",
         "-r:" + os.path.join(MANAGED, "mscorlib.dll"),
         "-out:" + os.path.join(MANAGED, "PS2.UnityShim.dll"),
         ] + shim_sources(), "csc PS2.UnityShim")

    run(["dotnet", "exec", CSC,
         "-nostdlib", "-noconfig", "-deterministic+", "-optimize+",
         "-target:library",
         "-r:" + os.path.join(MANAGED, "mscorlib.dll"),
         "-r:" + os.path.join(MANAGED, "PS2.UnityShim.dll"),
         "-out:" + os.path.join(MANAGED, "Assembly-CSharp.dll"),
         ] + game_sources(), "csc Assembly-CSharp (user scripts)")

    link_xml_path = os.path.join(WORK, "link.xml")
    with open(link_xml_path, "w", newline="\n") as f:
        f.write(LINK_XML)

    if os.path.isdir(STRIPPED):
        shutil.rmtree(STRIPPED)
    run([
        DEPLOY + "/UnityLinker.exe",
        "--include-assembly=" + os.path.join(MANAGED, "Assembly-CSharp.dll"),
        "--include-link-xml=" + link_xml_path,
        "--search-directory=" + MANAGED,
        "--out=" + STRIPPED,
        "--core-action=link",
        "--i18n=none",
        "--rule-set=aggressive",
    ], "UnityLinker strip")

    if os.path.isdir(GEN):
        shutil.rmtree(GEN)
    run([
        DEPLOY + "/il2cpp.exe",
        "--convert-to-cpp",
        "--directory=" + STRIPPED,
        "--generatedcppdir=" + GEN,
        "--dotnetprofile=unityaot-win32",
        "--emit-null-checks=false",
        "--enable-array-bounds-check=false",
        "--enable-divide-by-zero-check=false",
    ], "il2cpp convert-to-cpp")

    print("[build_m7] OK: generated sources in " + GEN)
    print("[build_m7] next: cmake -G Ninja -S il2cpp-port -B build/m7-ee "
          "-DCMAKE_TOOLCHAIN_FILE=tools/cmake/ps2-toolchain.cmake "
          "-DM6_GENERATED=" + GEN + " -DM6_MAIN=main_m7.cpp "
          "-DPS2UR_RUNTIME=" + os.path.join(REPO, "build", "ps2-release", "runtime"))


if __name__ == "__main__":
    main()
