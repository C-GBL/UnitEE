using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using UnityEditor;
using UnityEngine;
using Debug = UnityEngine.Debug;

namespace Ps2.Editor
{
    /// <summary>
    /// Machine-readable build report, written next to the output as
    /// build-report.json (plan section 13.4).
    /// </summary>
    [Serializable]
    public sealed class PS2BuildReport
    {
        [Serializable]
        public sealed class Step
        {
            public string name;
            public string outcome;
            public long milliseconds;
            public string message;
        }

        public string packageVersion;
        public string profileName;
        public string timestampUtc;
        public bool succeeded;
        public long totalMilliseconds;
        public string failureMessage = "";
        public string elfPath = "";
        public string isoPath = "";
        public long elfBytes;
        public long isoBytes;
        public List<Step> steps = new List<Step>();
        public List<string> warnings = new List<string>();
    }

    /// <summary>
    /// The build orchestrator (plan section 13.3): discrete, individually
    /// re-runnable, individually cacheable steps.
    ///
    /// The value of the step list being DATA rather than a hard-coded sequence
    /// of method calls is that the window can show it, the report can name
    /// which one failed, and a caller can run one in isolation. "The build
    /// failed" is not a bug report; "step 6 of 8 failed and here is the
    /// compiler output" is.
    /// </summary>
    public static class PS2BuildPipeline
    {
        public const string FallbackPackageVersion = "0.12.0";
        private const string LogPrefix = "[PS2 Build] ";

        /// <summary>
        /// The build steps, in order. The plan lists ten; export-scenes and
        /// export-assets are one step here because the exporter walks a scene
        /// and emits its meshes, textures, animation, audio and colliders into
        /// ONE container -- splitting them would mean opening every scene
        /// twice. Deploy is appended only for Build and Run.
        /// </summary>
        public static IPS2BuildStep[] CreateSteps()
        {
            return new IPS2BuildStep[]
            {
                new PS2StepValidate(),
                new PS2StepResolveContent(),
                new PS2StepExportScenes(),
                new PS2StepCompileManaged(),
                new PS2StepRunIl2cpp(),
                new PS2StepGenerateBridge(),
                new PS2StepNativeBuild(),
                new PS2StepPackage(),
            };
        }

        public static PS2BuildReport Build(PS2BuildProfile profile, bool run = false,
                                           bool forceRebuild = false)
        {
            if (profile == null)
                throw new PS2BuildException("No build profile was supplied.");

            PS2BuildContext ctx = CreateContext(profile, forceRebuild);
            var report = new PS2BuildReport
            {
                packageVersion = GetPackageVersion(),
                profileName = profile.name,
                timestampUtc = DateTime.UtcNow.ToString("o"),
            };

            var steps = CreateSteps().ToList();
            if (run)
                steps.Add(new PS2StepDeploy());

            var total = Stopwatch.StartNew();

            try
            {
                for (int i = 0; i < steps.Count; i++)
                {
                    IPS2BuildStep step = steps[i];
                    float progress = (float)i / steps.Count;
                    if (ShowProgress(step.Name, i + 1, steps.Count, progress))
                        throw new PS2BuildException("The build was cancelled.");

                    var watch = Stopwatch.StartNew();
                    var result = new PS2StepResult { name = step.Name };
                    bool upToDate;
                    try
                    {
                        upToDate = step.IsUpToDate(ctx);
                    }
                    catch (Exception e)
                    {
                        // A cache check that throws must not fail the build --
                        // it only means the cache cannot be trusted, and
                        // rebuilding is always the safe direction.
                        ctx.Warn($"{step.Name}: the up-to-date check failed " +
                                 $"({e.Message}); the step will run.");
                        upToDate = false;
                    }

                    if (upToDate)
                    {
                        result.outcome = PS2StepOutcome.Skipped;
                        result.message = "up to date";
                        Debug.Log($"{LogPrefix}{i + 1}/{steps.Count} {step.Name}: up to date");
                    }
                    else
                    {
                        step.Run(ctx);
                        result.outcome = PS2StepOutcome.Ran;
                        Debug.Log($"{LogPrefix}{i + 1}/{steps.Count} {step.Name}: " +
                                  $"{watch.ElapsedMilliseconds} ms");
                    }
                    result.milliseconds = watch.ElapsedMilliseconds;
                    ctx.Results.Add(result);
                }
                report.succeeded = true;
            }
            catch (Exception e)
            {
                report.succeeded = false;
                report.failureMessage = e.Message;
                ctx.Results.Add(new PS2StepResult
                {
                    name = ctx.Results.Count < steps.Count
                               ? steps[ctx.Results.Count].Name
                               : "(unknown)",
                    outcome = PS2StepOutcome.Failed,
                    message = e.Message,
                });
                Debug.LogError(LogPrefix + e.Message);
            }
            finally
            {
                EditorUtility.ClearProgressBar();
                total.Stop();
                SaveCache(ctx);
            }

            report.totalMilliseconds = total.ElapsedMilliseconds;
            foreach (PS2StepResult r in ctx.Results)
            {
                report.steps.Add(new PS2BuildReport.Step
                {
                    name = r.name,
                    outcome = r.outcome.ToString(),
                    milliseconds = r.milliseconds,
                    message = r.message,
                });
            }
            report.warnings.AddRange(ctx.Warnings);
            if (File.Exists(ctx.ElfPath))
            {
                report.elfPath = ctx.ElfPath;
                report.elfBytes = new FileInfo(ctx.ElfPath).Length;
            }
            if (File.Exists(ctx.IsoPath))
            {
                report.isoPath = ctx.IsoPath;
                report.isoBytes = new FileInfo(ctx.IsoPath).Length;
            }

            // Every step ran without throwing -- but did the build actually
            // produce a game?
            //
            // This check exists because it has been wrong twice. Once the
            // native step warned instead of failing, so a compile error came
            // back as a green build with an empty elfPath. Once the content
            // list was populated inside a cacheable step, so an incremental
            // build shipped an ELF with no scene beside it and the console
            // showed a black screen. Both times every step reported success
            // and the output directory was incomplete, and both times the
            // person who found it was the user, running it.
            //
            // So success is defined here as "the output is loadable", not as
            // "nothing threw".
            if (report.succeeded)
            {
                var missing = new List<string>();
                if (report.elfBytes == 0)
                    missing.Add("the game ELF");
                foreach (string content in ctx.ContentFiles)
                {
                    string beside = Path.Combine(ctx.OutputDirectory,
                                                 Path.GetFileName(content));
                    if (!File.Exists(beside))
                        missing.Add(Path.GetFileName(content));
                }
                if (missing.Count > 0)
                {
                    report.succeeded = false;
                    report.failureMessage =
                        "Every build step reported success, but the output " +
                        "directory is missing " + string.Join(", ", missing) +
                        ". The build is not runnable, so it is reported as a " +
                        "failure rather than a green build that shows a black " +
                        "screen.";
                    Debug.LogError(LogPrefix + report.failureMessage);
                }
            }

            WriteReport(ctx, report);
            LogSummary(report);
            return report;
        }

        public static PS2BuildReport BuildAndRun(PS2BuildProfile profile) =>
            Build(profile, run: true);

        /// <summary>
        /// Deletes the intermediates and the output. Deliberately does NOT
        /// touch anything else: a Clean that removes something the user put in
        /// the output folder by hand is a Clean nobody presses twice.
        /// </summary>
        public static void Clean(PS2BuildProfile profile)
        {
            PS2BuildContext ctx = CreateContext(profile, forceRebuild: true);
            foreach (string dir in new[] { ctx.IntermediateDirectory, ctx.OutputDirectory })
            {
                if (!Directory.Exists(dir))
                    continue;
                Directory.Delete(dir, true);
                Debug.Log($"{LogPrefix}removed {dir}");
            }
        }

        // ---- command line (plan 13.4) -------------------------------------

        /// <summary>
        /// Batch-mode entry point:
        ///
        ///   Unity -batchmode -quit -projectPath &lt;proj&gt; \
        ///         -executeMethod Ps2.Editor.PS2BuildPipeline.BuildFromCommandLine \
        ///         -ps2Profile Assets/Settings/PS2/Release.asset \
        ///         -ps2Output Build/PS2/
        ///
        /// Exits non-zero on any failure. Unity swallows an exception thrown
        /// out of -executeMethod and still exits 0, so the exit code is set
        /// explicitly -- otherwise a broken build looks like a passing one to
        /// CI, which is worse than having no CI at all.
        /// </summary>
        public static void BuildFromCommandLine()
        {
            int exitCode = 1;
            try
            {
                string profilePath = ArgumentValue("-ps2Profile");
                string output = ArgumentValue("-ps2Output");
                bool run = HasFlag("-ps2Run");
                bool force = HasFlag("-ps2ForceRebuild");

                if (string.IsNullOrEmpty(profilePath))
                {
                    throw new PS2BuildException(
                        "-ps2Profile is required, e.g. " +
                        "-ps2Profile Assets/Settings/PS2/Release.asset");
                }

                var profile = AssetDatabase.LoadAssetAtPath<PS2BuildProfile>(profilePath);
                if (profile == null)
                    throw new PS2BuildException($"No PS2BuildProfile at '{profilePath}'.");

                if (!string.IsNullOrEmpty(output))
                    profile.outputDirectory = output;

                PS2BuildReport report = Build(profile, run, force);
                exitCode = report.succeeded ? 0 : 1;
            }
            catch (Exception e)
            {
                Debug.LogError(LogPrefix + e);
                exitCode = 1;
            }
            if (Application.isBatchMode)
                EditorApplication.Exit(exitCode);
        }

        private static string ArgumentValue(string name)
        {
            string[] args = Environment.GetCommandLineArgs();
            for (int i = 0; i < args.Length - 1; i++)
            {
                if (string.Equals(args[i], name, StringComparison.OrdinalIgnoreCase))
                    return args[i + 1];
            }
            return null;
        }

        private static bool HasFlag(string name) =>
            Environment.GetCommandLineArgs().Any(
                a => string.Equals(a, name, StringComparison.OrdinalIgnoreCase));

        // ---- helpers ------------------------------------------------------

        public static PS2BuildContext CreateContext(PS2BuildProfile profile,
                                                    bool forceRebuild)
        {
            string projectRoot = Path.GetDirectoryName(Application.dataPath);
            // Library/, NOT Temp/. The plan says Temp/PS2Build, but Unity
            // DELETES Temp/ when the Editor exits -- so in batch mode (which
            // is how CI builds) every intermediate and the whole incremental
            // cache vanish between runs, and "incremental" silently means
            // "always a clean build". Library/ survives, is already
            // gitignored, and is where Unity keeps its own build caches.
            string intermediate = Path.Combine(projectRoot, "Library", "PS2Build");
            var ctx = new PS2BuildContext
            {
                Profile = profile,
                ProjectRoot = projectRoot,
                OutputDirectory = ResolveOutputDirectory(profile),
                IntermediateDirectory = intermediate,
                PackageRoot = FindPackageRoot(projectRoot),
                ForceRebuild = forceRebuild,
            };
            ctx.Cache = PS2BuildCache.Load(CachePath(ctx));
            return ctx;
        }

        private static string CachePath(PS2BuildContext ctx) =>
            Path.Combine(ctx.IntermediateDirectory, "build-cache.json");

        private static void SaveCache(PS2BuildContext ctx)
        {
            try
            {
                ctx.Cache?.Save(CachePath(ctx));
            }
            catch (Exception e)
            {
                Debug.LogWarning(LogPrefix + "the build cache could not be saved: " +
                                 e.Message);
            }
        }

        /// <summary>Resolves the profile's output directory against the project root when relative.</summary>
        public static string ResolveOutputDirectory(PS2BuildProfile profile)
        {
            if (profile == null || string.IsNullOrWhiteSpace(profile.outputDirectory))
                throw new PS2BuildException("The build profile's output directory is not set.");
            if (Path.IsPathRooted(profile.outputDirectory))
                return Path.GetFullPath(profile.outputDirectory);
            string projectRoot = Path.GetDirectoryName(Application.dataPath);
            return Path.GetFullPath(Path.Combine(projectRoot, profile.outputDirectory));
        }

        /// <summary>
        /// The repository root, which holds runtime/, tools/ and the CMake
        /// tree.
        ///
        /// Found from the PACKAGE's own location, not from the project's. The
        /// package is wired in with a `file:` dependency during development
        /// and the repository can be anywhere on disk -- walking up from the
        /// Unity project only works when the two happen to be neighbours, and
        /// silently failing to find tools/ is how mkps2iso goes missing on a
        /// machine that has it.
        /// </summary>
        public static string FindPackageRoot(string projectRoot)
        {
            try
            {
                UnityEditor.PackageManager.PackageInfo info =
                    UnityEditor.PackageManager.PackageInfo.FindForAssembly(
                        typeof(PS2BuildPipeline).Assembly);
                if (info != null && !string.IsNullOrEmpty(info.resolvedPath))
                {
                    string found = WalkUpForMarkers(info.resolvedPath);
                    if (found != null)
                        return found;
                }
            }
            catch (Exception)
            {
                // Not installed as a package (scripts copied into Assets/):
                // fall through to the project-relative search.
            }

            // The assembly's own file location, which works when the package
            // is a plain folder rather than a resolved UPM package.
            try
            {
                string assemblyPath = typeof(PS2BuildPipeline).Assembly.Location;
                if (!string.IsNullOrEmpty(assemblyPath))
                {
                    string found = WalkUpForMarkers(Path.GetDirectoryName(assemblyPath));
                    if (found != null)
                        return found;
                }
            }
            catch (Exception)
            {
            }

            string fromProject = WalkUpForMarkers(projectRoot);
            if (fromProject != null)
                return fromProject;

            // Siblings of the project, the layout a `file:../repo` dependency
            // produces when the two ARE neighbours.
            var parent = Directory.GetParent(projectRoot);
            if (parent != null)
            {
                foreach (DirectoryInfo sibling in parent.GetDirectories())
                {
                    if (File.Exists(Path.Combine(sibling.FullName, "ps2port.txt")))
                        return sibling.FullName.Replace('\\', '/');
                }
            }
            return projectRoot.Replace('\\', '/');
        }

        private static string WalkUpForMarkers(string start)
        {
            var dir = new DirectoryInfo(start);
            while (dir != null)
            {
                if (Directory.Exists(Path.Combine(dir.FullName, "runtime")) &&
                    Directory.Exists(Path.Combine(dir.FullName, "tools")))
                {
                    return dir.FullName.Replace('\\', '/');
                }
                dir = dir.Parent;
            }
            return null;
        }

        private static bool ShowProgress(string step, int index, int count, float progress)
        {
            // No UI in batch mode, and a cancelable progress bar there would
            // block forever waiting for a click that cannot happen.
            if (Application.isBatchMode)
                return false;
            return EditorUtility.DisplayCancelableProgressBar(
                "Building for PlayStation 2",
                $"Step {index} of {count}: {step}", progress);
        }

        private static void WriteReport(PS2BuildContext ctx, PS2BuildReport report)
        {
            try
            {
                Directory.CreateDirectory(ctx.OutputDirectory);
                File.WriteAllText(Path.Combine(ctx.OutputDirectory, "build-report.json"),
                                  JsonUtility.ToJson(report, true));
            }
            catch (Exception e)
            {
                Debug.LogWarning(LogPrefix + "the build report could not be written: " +
                                 e.Message);
            }
        }

        private static void LogSummary(PS2BuildReport report)
        {
            var sb = new StringBuilder();
            sb.AppendLine(report.succeeded
                              ? $"{LogPrefix}SUCCEEDED in {report.totalMilliseconds} ms"
                              : $"{LogPrefix}FAILED after {report.totalMilliseconds} ms");
            foreach (PS2BuildReport.Step step in report.steps)
                sb.AppendLine($"  {step.outcome,-8} {step.milliseconds,6} ms  {step.name}");
            if (report.elfBytes > 0)
                sb.AppendLine($"  ELF {report.elfBytes / 1024} KB  {report.elfPath}");
            if (report.isoBytes > 0)
                sb.AppendLine($"  ISO {report.isoBytes / 1024} KB  {report.isoPath}");
            if (report.warnings.Count > 0)
                sb.AppendLine($"  {report.warnings.Count} warning(s)");
            if (report.succeeded)
                Debug.Log(sb.ToString());
            else
                Debug.LogError(sb.ToString());
        }

        private static string GetPackageVersion()
        {
            try
            {
                UnityEditor.PackageManager.PackageInfo info =
                    UnityEditor.PackageManager.PackageInfo.FindForAssembly(
                        typeof(PS2BuildPipeline).Assembly);
                if (info != null && !string.IsNullOrEmpty(info.version))
                    return info.version;
            }
            catch (Exception)
            {
                // Fall through to the compiled-in version.
            }
            return FallbackPackageVersion;
        }
    }
}
