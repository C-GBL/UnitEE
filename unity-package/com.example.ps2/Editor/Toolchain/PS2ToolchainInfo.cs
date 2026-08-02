using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Ps2.Editor
{
    /// <summary>
    /// A resolved snapshot of everything a build needs from outside the project
    /// (plan section 13.3 task 4). Gathered once at the top of a build so a
    /// missing tool fails in the first second rather than forty minutes in,
    /// after IL2CPP has run.
    ///
    /// This is the C# equivalent of tools/ps2dev/doctor.sh, and it is
    /// deliberately the same checks: if the shell script says the toolchain is
    /// healthy and this says it is not, one of them is wrong and that is worth
    /// knowing.
    /// </summary>
    public sealed class PS2ToolchainInfo
    {
        public string Ps2DevRoot;
        public string Ps2SdkDir;
        public string EeGxx;
        public string Il2cppExe;
        public string LibIl2cppDir;
        public string UnityAotBclDir;
        public string CMakeExe;
        public string NinjaExe;
        public string PythonExe;
        public string Pcsx2Exe;
        public string MkPs2IsoExe;
        public string Ps2ClientExe;

        public readonly List<string> Problems = new List<string>();
        public readonly List<string> Notes = new List<string>();

        /// <summary>Everything required to produce an ELF is present.</summary>
        public bool CanBuild => Problems.Count == 0;

        /// <summary>
        /// Resolves the toolchain. 'packageRoot' is the repository root, used
        /// to find the checked-in tools (bindgen, the layout planner) and the
        /// locally installed mkps2iso.
        /// </summary>
        public static PS2ToolchainInfo Discover(string packageRoot)
        {
            var info = new PS2ToolchainInfo();

            PS2ToolResult ps2dev = PS2ToolchainLocator.Ps2DevRoot;
            if (ps2dev.ok)
                info.Ps2DevRoot = ps2dev.path;
            else
                info.Problems.Add(ps2dev.error);

            PS2ToolResult sdk = PS2ToolchainLocator.Ps2SdkDir;
            if (sdk.ok)
                info.Ps2SdkDir = sdk.path;
            else
                info.Problems.Add(sdk.error);

            PS2ToolResult gxx = PS2ToolchainLocator.EeGxx;
            if (gxx.ok)
                info.EeGxx = gxx.path;
            else
                info.Problems.Add(gxx.error);

            PS2ToolResult il2cpp = PS2ToolchainLocator.Il2cppExe;
            if (il2cpp.ok)
                info.Il2cppExe = il2cpp.path;
            else
                info.Problems.Add(il2cpp.error);

            PS2ToolResult libil2cpp = PS2ToolchainLocator.LibIl2cppDir;
            if (libil2cpp.ok)
                info.LibIl2cppDir = libil2cpp.path;
            else
                info.Problems.Add(libil2cpp.error);

            PS2ToolResult bcl = PS2ToolchainLocator.UnityAotBclDir;
            if (bcl.ok)
                info.UnityAotBclDir = bcl.path;
            else
                info.Problems.Add(bcl.error);

            info.CMakeExe = FindOnPath("cmake");
            if (info.CMakeExe == null)
                info.Problems.Add(
                    "cmake was not found on PATH. The native build is CMake+Ninja " +
                    "(plan 4.1); install CMake 3.24 or newer.");

            info.NinjaExe = FindOnPath("ninja");
            if (info.NinjaExe == null)
                info.Notes.Add(
                    "ninja was not found on PATH; CMake will fall back to its " +
                    "default generator, which is slower and does not stream output " +
                    "as well.");

            info.PythonExe = FindOnPath("python") ?? FindOnPath("python3");
            if (info.PythonExe == null)
                info.Problems.Add(
                    "python was not found on PATH. The bridge generator " +
                    "(tools/bindgen) and the disc layout planner are Python.");

            PS2ToolResult pcsx2 = PS2ToolchainLocator.Pcsx2Exe;
            if (pcsx2.ok)
                info.Pcsx2Exe = pcsx2.path;
            else
                info.Notes.Add(
                    "PCSX2 was not found; Build works, Build and Run to the " +
                    "emulator does not. " + pcsx2.error);

            // mkps2iso is a third-party binary the developer installs; it is
            // gitignored on purpose (it is not ours to vendor).
            if (!string.IsNullOrEmpty(packageRoot))
            {
                string mk = Path.Combine(packageRoot, "tools/mkps2iso/mkps2iso.exe");
                if (File.Exists(mk))
                    info.MkPs2IsoExe = mk.Replace('\\', '/');
            }
            if (info.MkPs2IsoExe == null)
                info.MkPs2IsoExe = FindOnPath("mkps2iso");
            if (info.MkPs2IsoExe == null)
                info.Notes.Add(
                    "mkps2iso was not found, so an ISO cannot be produced. Install " +
                    "it to tools/mkps2iso/ or put it on PATH; game.elf still builds " +
                    "and runs over host: in PCSX2.");

            info.Ps2ClientExe = FindOnPath("ps2client");
            return info;
        }

        /// <summary>A doctor-style report, for the window and the console.</summary>
        public string Report()
        {
            var sb = new StringBuilder();
            sb.AppendLine("PS2 toolchain:");
            Line(sb, "PS2DEV", Ps2DevRoot);
            Line(sb, "ps2sdk", Ps2SdkDir);
            Line(sb, "EE g++", EeGxx);
            Line(sb, "il2cpp", Il2cppExe);
            Line(sb, "libil2cpp", LibIl2cppDir);
            Line(sb, "AOT BCL", UnityAotBclDir);
            Line(sb, "cmake", CMakeExe);
            Line(sb, "ninja", NinjaExe);
            Line(sb, "python", PythonExe);
            Line(sb, "PCSX2", Pcsx2Exe);
            Line(sb, "mkps2iso", MkPs2IsoExe);
            foreach (string p in Problems)
                sb.AppendLine("  ERROR: " + p);
            foreach (string n in Notes)
                sb.AppendLine("  note:  " + n);
            return sb.ToString();
        }

        private static void Line(StringBuilder sb, string label, string value)
        {
            sb.AppendLine($"  {label,-10} {(string.IsNullOrEmpty(value) ? "MISSING" : value)}");
        }

        private static string FindOnPath(string exe)
        {
            string path = Environment.GetEnvironmentVariable("PATH");
            if (string.IsNullOrEmpty(path))
                return null;
            string[] suffixes = Environment.OSVersion.Platform == PlatformID.Win32NT
                ? new[] { ".exe", ".cmd", ".bat", "" }
                : new[] { "" };
            foreach (string dir in path.Split(Path.PathSeparator))
            {
                if (string.IsNullOrEmpty(dir))
                    continue;
                foreach (string suffix in suffixes)
                {
                    string candidate;
                    try
                    {
                        candidate = Path.Combine(dir.Trim('"'), exe + suffix);
                    }
                    catch (ArgumentException)
                    {
                        continue; // a malformed PATH entry is not fatal
                    }
                    if (File.Exists(candidate))
                        return candidate.Replace('\\', '/');
                }
            }
            return null;
        }
    }
}
