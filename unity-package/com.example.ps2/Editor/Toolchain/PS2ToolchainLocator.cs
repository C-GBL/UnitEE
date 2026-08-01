using System;
using System.IO;
using UnityEditor;

namespace Ps2.Editor
{
    /// <summary>
    /// Result of a tool lookup. Getters on <see cref="PS2ToolchainLocator"/> never
    /// throw; callers inspect <see cref="ok"/> and surface <see cref="error"/> as an
    /// actionable message (validator warning or PS2BuildException).
    /// </summary>
    public sealed class PS2ToolResult
    {
        public readonly bool ok;
        public readonly string path;
        public readonly string error;

        private PS2ToolResult(bool ok, string path, string error)
        {
            this.ok = ok;
            this.path = path;
            this.error = error;
        }

        public static PS2ToolResult Success(string path)
        {
            return new PS2ToolResult(true, path, null);
        }

        public static PS2ToolResult Failure(string error)
        {
            return new PS2ToolResult(false, null, error);
        }
    }

    /// <summary>
    /// Locates every external tool the pipeline needs: Unity's il2cpp compiler and
    /// support directories (inside the running Editor install), the ps2dev
    /// cross-toolchain, and PCSX2 for Build And Run.
    /// </summary>
    public static class PS2ToolchainLocator
    {
        /// <summary>Default ps2dev install root when the PS2DEV env var is not set.</summary>
        public const string DefaultPs2DevRoot = "C:/Users/Ash/ps2dev";

        /// <summary>
        /// il2cpp AOT compiler. Verified layout for Unity 6000.0.47f1:
        /// &lt;EditorData&gt;/il2cpp/build/deploy/il2cpp.exe -- the exe sits directly
        /// in deploy/, there is no net* subfolder in this version (the plan's
        /// section 1.2 [VERIFY] note resolved).
        /// </summary>
        public static PS2ToolResult Il2cppExe
        {
            get
            {
                string deployDir = EditorApplication.applicationContentsPath + "/il2cpp/build/deploy";
                string exe = deployDir + "/il2cpp.exe";
                if (File.Exists(exe))
                {
                    return PS2ToolResult.Success(exe);
                }
                string noExt = deployDir + "/il2cpp";
                if (File.Exists(noExt))
                {
                    return PS2ToolResult.Success(noExt);
                }
                return PS2ToolResult.Failure(
                    "il2cpp compiler not found at '" + exe + "'. Verified layout for Unity 6000.0.47f1 is " +
                    "<EditorData>/il2cpp/build/deploy/il2cpp.exe (directly in deploy/, no net* subfolder). " +
                    "Install the IL2CPP scripting backend component for this Editor via Unity Hub.");
            }
        }

        /// <summary>libil2cpp CLR source shipped with the Editor (plan section 1.2). Never copied into the repo.</summary>
        public static PS2ToolResult LibIl2cppDir
        {
            get
            {
                string dir = EditorApplication.applicationContentsPath + "/il2cpp/libil2cpp";
                if (Directory.Exists(dir))
                {
                    return PS2ToolResult.Success(dir);
                }
                return PS2ToolResult.Failure(
                    "libil2cpp source not found at '" + dir + "'. Install the IL2CPP scripting backend " +
                    "component for this Editor via Unity Hub.");
            }
        }

        /// <summary>AOT base class library il2cpp compiles against (plan section 4.2). Windows Editor ships unityaot-win32.</summary>
        public static PS2ToolResult UnityAotBclDir
        {
            get
            {
                string dir = EditorApplication.applicationContentsPath + "/MonoBleedingEdge/lib/mono/unityaot-win32";
                if (Directory.Exists(dir))
                {
                    return PS2ToolResult.Success(dir);
                }
                return PS2ToolResult.Failure(
                    "unityaot BCL not found at '" + dir + "'. Verified layout for Unity 6000.0.47f1 is " +
                    "<EditorData>/MonoBleedingEdge/lib/mono/unityaot-win32 (unityaot-linux and unityaot-macos " +
                    "also ship). Install an IL2CPP-capable desktop platform module via Unity Hub.");
            }
        }

        /// <summary>
        /// ps2dev toolchain root: PS2DEV env var, else <see cref="DefaultPs2DevRoot"/>.
        /// Plan section 4.1: the path must be absolute, without spaces or non-Latin
        /// characters. PS2SDK = $PS2DEV/ps2sdk, GSKIT = $PS2DEV/gsKit.
        /// </summary>
        public static PS2ToolResult Ps2DevRoot
        {
            get
            {
                string root = Environment.GetEnvironmentVariable("PS2DEV");
                if (string.IsNullOrEmpty(root))
                {
                    root = DefaultPs2DevRoot;
                }
                if (Directory.Exists(root))
                {
                    return PS2ToolResult.Success(root);
                }
                return PS2ToolResult.Failure(
                    "ps2dev toolchain not found at '" + root + "'. Run tools/ps2dev/install.ps1 " +
                    "(the toolchain install may still be in progress) or set the PS2DEV environment variable.");
            }
        }

        /// <summary>PS2SDK root derived from <see cref="Ps2DevRoot"/>.</summary>
        public static PS2ToolResult Ps2SdkDir
        {
            get
            {
                PS2ToolResult root = Ps2DevRoot;
                if (!root.ok)
                {
                    return root;
                }
                string dir = root.path + "/ps2sdk";
                if (Directory.Exists(dir))
                {
                    return PS2ToolResult.Success(dir);
                }
                return PS2ToolResult.Failure(
                    "ps2sdk not found at '" + dir + "'. Run tools/ps2dev/install.ps1 to complete the toolchain install.");
            }
        }

        /// <summary>gsKit root derived from <see cref="Ps2DevRoot"/> (reference/bring-up renderer per ADR-003).</summary>
        public static PS2ToolResult GsKitDir
        {
            get
            {
                PS2ToolResult root = Ps2DevRoot;
                if (!root.ok)
                {
                    return root;
                }
                string dir = root.path + "/gsKit";
                if (Directory.Exists(dir))
                {
                    return PS2ToolResult.Success(dir);
                }
                return PS2ToolResult.Failure(
                    "gsKit not found at '" + dir + "'. Run tools/ps2dev/install.ps1 to complete the toolchain install.");
            }
        }

        /// <summary>EE C++ cross-compiler: $PS2DEV/ee/bin/mips64r5900el-ps2-elf-g++ (plan section 4.1 tool table).</summary>
        public static PS2ToolResult EeGxx
        {
            get
            {
                PS2ToolResult root = Ps2DevRoot;
                if (!root.ok)
                {
                    return root;
                }
                string baseName = root.path + "/ee/bin/mips64r5900el-ps2-elf-g++";
                if (File.Exists(baseName + ".exe"))
                {
                    return PS2ToolResult.Success(baseName + ".exe");
                }
                if (File.Exists(baseName))
                {
                    return PS2ToolResult.Success(baseName);
                }
                return PS2ToolResult.Failure(
                    "mips64r5900el-ps2-elf-g++ not found under '" + root.path + "/ee/bin'. " +
                    "Run tools/ps2dev/install.ps1 or set the PS2DEV environment variable to a complete install.");
            }
        }

        /// <summary>
        /// PCSX2 emulator for Build And Run. Probes the PCSX2 env var (file, or a
        /// directory containing pcsx2-qt.exe), then common install locations.
        /// CLI flags used by the pipeline (plan section 4.1):
        /// -batch -nogui -elf &lt;file&gt; -logfile &lt;path&gt; -fastboot.
        /// </summary>
        public static PS2ToolResult Pcsx2Exe
        {
            get
            {
                string env = Environment.GetEnvironmentVariable("PCSX2");
                if (!string.IsNullOrEmpty(env))
                {
                    if (File.Exists(env))
                    {
                        return PS2ToolResult.Success(env);
                    }
                    if (Directory.Exists(env))
                    {
                        string inDir = Path.Combine(env, "pcsx2-qt.exe");
                        if (File.Exists(inDir))
                        {
                            return PS2ToolResult.Success(inDir);
                        }
                    }
                }

                string[] candidates = new string[]
                {
                    "C:/Program Files/PCSX2/pcsx2-qt.exe",
                    "C:/Program Files/PCSX2/pcsx2.exe",
                    "C:/Program Files (x86)/PCSX2/pcsx2.exe"
                };
                string localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
                if (!string.IsNullOrEmpty(localAppData))
                {
                    candidates = new string[]
                    {
                        candidates[0],
                        candidates[1],
                        candidates[2],
                        localAppData + "/Programs/PCSX2/pcsx2-qt.exe"
                    };
                }
                for (int i = 0; i < candidates.Length; i++)
                {
                    if (File.Exists(candidates[i]))
                    {
                        return PS2ToolResult.Success(candidates[i]);
                    }
                }
                return PS2ToolResult.Failure(
                    "PCSX2 not found. Set the PCSX2 environment variable to the full path of pcsx2-qt.exe " +
                    "(or its install directory), or install PCSX2 to C:/Program Files/PCSX2.");
            }
        }
    }
}
