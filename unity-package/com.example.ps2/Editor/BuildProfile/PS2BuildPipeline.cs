using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using UnityEditor;
using UnityEngine;
using Debug = UnityEngine.Debug;

namespace Ps2.Editor
{
    /// <summary>
    /// Snapshot of the settings a build was made with, written as
    /// build-manifest.json to the output directory by the PrepareOutput stage.
    /// </summary>
    [Serializable]
    public sealed class PS2BuildManifest
    {
        public string packageVersion;
        public string profileName;
        public string timestampUtc;
        public string region;
        public string videoMode;
        public int textureBudgetKb;
        public bool buildIso;
        public string outputDirectory;
        public string[] scriptingDefines;
        public string[] scenePaths;
    }

    /// <summary>
    /// Static build orchestrator. Stage order follows the plan section 6 diagram:
    /// Validate -> ExportScenes -> ExportAssets -> CollectAndCompileScripts ->
    /// RunIl2cpp -> NativeBuild -> Package.
    ///
    /// Implemented today: validation, output directory creation, build-manifest.json,
    /// per-stage logging with a stopwatch. Every unimplemented stage throws
    /// <see cref="PS2BuildException"/> with an actionable message so Build fails
    /// loudly instead of emitting a broken ISO (plan section 7 requirement).
    /// </summary>
    public static class PS2BuildPipeline
    {
        private const string LogPrefix = "[PS2] ";
        private const string FallbackPackageVersion = "0.1.0";

        public static void Build(PS2BuildProfile profile)
        {
            Stopwatch total = Stopwatch.StartNew();

            RunStage("Validate", () => Validate(profile));

            string outDir = ResolveOutputDirectory(profile);
            RunStage("PrepareOutput", () => PrepareOutput(profile, outDir));
            RunStage("ExportScenes", () => ExportScenes(profile, outDir));
            RunStage("ExportAssets", () => ExportAssets(profile, outDir));
            RunStage("CollectAndCompileScripts", () => CollectAndCompileScripts(profile, outDir));
            RunStage("RunIl2cpp", () => RunIl2cpp(profile, outDir));
            RunStage("NativeBuild", () => NativeBuild(profile, outDir));
            RunStage("Package", () => Package(profile, outDir));

            total.Stop();
            Debug.Log(LogPrefix + "build finished in " + total.ElapsedMilliseconds + " ms -> " + outDir);
        }

        public static void BuildAndRun(PS2BuildProfile profile)
        {
            // Locate the emulator up front so a missing PCSX2 fails before a long build.
            PS2ToolResult pcsx2 = PS2ToolchainLocator.Pcsx2Exe;
            if (!pcsx2.ok)
            {
                throw new PS2BuildException("Build And Run requires PCSX2. " + pcsx2.error);
            }

            Build(profile);

            string outDir = ResolveOutputDirectory(profile);
            RunInPcsx2(pcsx2.path, Path.Combine(outDir, "game.elf"), Path.Combine(outDir, "pcsx2.log"));
        }

        public static void Clean(PS2BuildProfile profile)
        {
            if (profile == null)
            {
                throw new PS2BuildException("Clean requires a build profile.");
            }
            if (string.IsNullOrEmpty(profile.outputDirectory))
            {
                throw new PS2BuildException("Clean: the profile's output directory is not set.");
            }

            string outDir = ResolveOutputDirectory(profile);
            if (!Directory.Exists(outDir))
            {
                Debug.Log(LogPrefix + "clean: nothing to remove at " + outDir);
                return;
            }

            string root = Path.GetPathRoot(outDir);
            string normalized = outDir.TrimEnd('\\', '/');
            if (string.Equals(normalized, root.TrimEnd('\\', '/'), StringComparison.OrdinalIgnoreCase))
            {
                throw new PS2BuildException("Clean: refusing to delete a drive root ('" + outDir + "'). Fix the profile's output directory.");
            }

            Directory.Delete(outDir, true);
            Debug.Log(LogPrefix + "clean: removed " + outDir);
        }

        // ------------------------------------------------------------------ stages

        private static void Validate(PS2BuildProfile profile)
        {
            List<PS2ValidationMessage> messages = PS2ProjectValidator.Validate(profile);
            List<string> errors = new List<string>();
            for (int i = 0; i < messages.Count; i++)
            {
                PS2ValidationMessage m = messages[i];
                if (m.severity == PS2ValidationSeverity.Error)
                {
                    errors.Add(m.message + " Fix: " + m.fixHint);
                }
                else if (m.severity == PS2ValidationSeverity.Warning)
                {
                    Debug.LogWarning(LogPrefix + m.message + " Fix: " + m.fixHint);
                }
            }
            if (errors.Count > 0)
            {
                throw new PS2BuildException("validation failed:\n - " + string.Join("\n - ", errors.ToArray()));
            }
        }

        private static void PrepareOutput(PS2BuildProfile profile, string outDir)
        {
            Directory.CreateDirectory(outDir);

            PS2BuildManifest manifest = new PS2BuildManifest
            {
                packageVersion = GetPackageVersion(),
                profileName = profile.name,
                timestampUtc = DateTime.UtcNow.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", CultureInfo.InvariantCulture),
                region = profile.region.ToString(),
                videoMode = profile.videoMode.ToString(),
                textureBudgetKb = profile.textureBudgetKb,
                buildIso = profile.buildIso,
                outputDirectory = outDir,
                scriptingDefines = profile.scriptingDefines != null ? profile.scriptingDefines : new string[0],
                scenePaths = CollectScenePaths(profile)
            };

            string manifestPath = Path.Combine(outDir, "build-manifest.json");
            File.WriteAllText(manifestPath, JsonUtility.ToJson(manifest, true));
            Debug.Log(LogPrefix + "wrote " + manifestPath);
        }

        private static void ExportScenes(PS2BuildProfile profile, string outDir)
        {
            string sceneOutDir = Path.Combine(outDir, "scenes");
            Directory.CreateDirectory(sceneOutDir);
            for (int i = 0; i < profile.scenes.Count; i++)
            {
                // Throws PS2BuildException today: blocked on the p2b container spec.
                PS2SceneExporter.ExportScene(profile.scenes[i], sceneOutDir);
            }
        }

        private static void ExportAssets(PS2BuildProfile profile, string outDir)
        {
            throw new PS2BuildException(
                "stage ExportAssets not implemented yet -- see docs/architecture.md. Asset conversion " +
                "(tri-stripper, VU1 batcher, palettiser + swizzler, anim quantiser, ADPCM encoder; plan " +
                "section 6) is blocked on the p2b container format. TODO(spec missing: section 10).");
        }

        private static void CollectAndCompileScripts(PS2BuildProfile profile, string outDir)
        {
            throw new PS2BuildException(
                "stage CollectAndCompileScripts not implemented yet -- see docs/architecture.md. This stage " +
                "collects .cs sources from the project's asmdefs and recompiles them with Roslyn against " +
                "PS2.UnityShim instead of UnityEngine (plan section 6 diagram). " +
                "TODO(spec missing: section 12.3).");
        }

        private static void RunIl2cpp(PS2BuildProfile profile, string outDir)
        {
            PS2ToolResult il2cpp = PS2ToolchainLocator.Il2cppExe;
            if (!il2cpp.ok)
            {
                throw new PS2BuildException(il2cpp.error);
            }
            Debug.Log(LogPrefix + "il2cpp resolved: " + il2cpp.path);

            throw new PS2BuildException(
                "stage RunIl2cpp not implemented yet -- see docs/architecture.md. Will invoke '" + il2cpp.path +
                "' with --convert-to-cpp (and without --compile-cpp) against the recompiled assemblies plus the " +
                "unityaot BCL (plan section 1.2), emitting portable C++ for the EE cross-compiler. " +
                "TODO(spec missing: section 11).");
        }

        private static void NativeBuild(PS2BuildProfile profile, string outDir)
        {
            throw new PS2BuildException(
                "stage NativeBuild not implemented yet -- see docs/architecture.md. Will configure CMake + Ninja " +
                "with tools/cmake/ps2-toolchain.cmake (EE compiler mips64r5900el-ps2-elf-g++ from $PS2DEV/ee/bin, " +
                "PS2DEV defaulting to " + PS2ToolchainLocator.DefaultPs2DevRoot + ") to compile il2cppOutput + " +
                "patched libil2cpp + bdwgc + the ps2ur runtime into game.elf (plan section 6). " +
                "TODO(spec missing: section 9).");
        }

        private static void Package(PS2BuildProfile profile, string outDir)
        {
            throw new PS2BuildException(
                "stage Package not implemented yet -- see docs/architecture.md. Will run ps2-packer on game.elf " +
                (profile.buildIso
                    ? "and mkps2iso (XML-scripted, controls file LBA order; plan section 4.1) to produce game.iso. "
                    : "(profile has Build ISO disabled). ") +
                "TODO(spec missing: section 9).");
        }

        // ------------------------------------------------------------------ helpers

        private static void RunStage(string name, Action stage)
        {
            Stopwatch sw = Stopwatch.StartNew();
            Debug.Log(LogPrefix + "stage start: " + name);
            try
            {
                stage();
            }
            catch (Exception)
            {
                sw.Stop();
                Debug.LogError(LogPrefix + "stage FAILED: " + name + " (" + sw.ElapsedMilliseconds + " ms)");
                throw;
            }
            sw.Stop();
            Debug.Log(LogPrefix + "stage ok: " + name + " (" + sw.ElapsedMilliseconds + " ms)");
        }

        /// <summary>Resolves the profile's output directory against the project root when relative.</summary>
        public static string ResolveOutputDirectory(PS2BuildProfile profile)
        {
            if (profile == null || string.IsNullOrEmpty(profile.outputDirectory))
            {
                throw new PS2BuildException("The build profile's output directory is not set.");
            }
            if (Path.IsPathRooted(profile.outputDirectory))
            {
                return Path.GetFullPath(profile.outputDirectory);
            }
            string projectRoot = Path.GetDirectoryName(Application.dataPath);
            return Path.GetFullPath(Path.Combine(projectRoot, profile.outputDirectory));
        }

        private static string[] CollectScenePaths(PS2BuildProfile profile)
        {
            List<string> paths = new List<string>();
            if (profile.scenes != null)
            {
                for (int i = 0; i < profile.scenes.Count; i++)
                {
                    if (profile.scenes[i] != null)
                    {
                        paths.Add(AssetDatabase.GetAssetPath(profile.scenes[i]));
                    }
                }
            }
            return paths.ToArray();
        }

        private static string GetPackageVersion()
        {
            try
            {
                UnityEditor.PackageManager.PackageInfo info =
                    UnityEditor.PackageManager.PackageInfo.FindForAssembly(typeof(PS2BuildPipeline).Assembly);
                if (info != null && !string.IsNullOrEmpty(info.version))
                {
                    return info.version;
                }
            }
            catch (Exception)
            {
                // Fall through to the compiled-in version.
            }
            return FallbackPackageVersion;
        }

        private static void RunInPcsx2(string pcsx2Path, string elfPath, string logPath)
        {
            // PCSX2 CLI flags per plan section 4.1 tool table:
            //   -batch -nogui -elf <file> -logfile <path> -fastboot
            if (!File.Exists(elfPath))
            {
                throw new PS2BuildException(
                    "Build And Run: no ELF at '" + elfPath + "'. The build did not produce output (earlier " +
                    "stages are not implemented yet).");
            }
            ProcessStartInfo psi = new ProcessStartInfo
            {
                FileName = pcsx2Path,
                Arguments = "-batch -nogui -fastboot -elf \"" + elfPath + "\" -logfile \"" + logPath + "\"",
                UseShellExecute = false
            };
            Debug.Log(LogPrefix + "launching PCSX2: \"" + pcsx2Path + "\" " + psi.Arguments);
            Process.Start(psi);
        }
    }
}
