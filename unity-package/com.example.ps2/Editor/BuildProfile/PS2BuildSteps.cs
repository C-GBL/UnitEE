using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace Ps2.Editor
{
    // The ten build steps of plan section 13.3, in order. Each is small and
    // separately re-runnable on purpose: a build that fails should name the
    // step, and a rebuild should redo only what changed.

    /// <summary>1. Validate -- toolchain, settings, project. Fail fast.</summary>
    public sealed class PS2StepValidate : IPS2BuildStep
    {
        public string Name => "Validate";

        // Validation is cheap and its whole job is catching a change someone
        // just made, so it never caches.
        public bool IsUpToDate(PS2BuildContext ctx) => false;

        public void Run(PS2BuildContext ctx)
        {
            ctx.Toolchain = PS2ToolchainInfo.Discover(ctx.PackageRoot);
            if (!ctx.Toolchain.CanBuild)
            {
                throw new PS2BuildException(
                    "The PS2 toolchain is incomplete:\n" + ctx.Toolchain.Report() +
                    "\nRun tools/ps2dev/install.ps1, or set PS2DEV to an existing install.");
            }
            foreach (string note in ctx.Toolchain.Notes)
                ctx.Warn(note);

            if (ctx.Profile.scenes == null || ctx.Profile.scenes.Count == 0)
                throw new PS2BuildException(
                    "The build profile has no scenes. Add at least one; the first " +
                    "scene in the list is the one that boots.");

            PS2ProfileValidator.Validate(ctx);
            PS2ProjectValidator.ValidateForBuild(ctx);
        }
    }

    /// <summary>2. Resolve content -- walk scenes, hash the closed asset set.</summary>
    public sealed class PS2StepResolveContent : IPS2BuildStep
    {
        public string Name => "Resolve content";

        public bool IsUpToDate(PS2BuildContext ctx) => false;

        public void Run(PS2BuildContext ctx)
        {
            ctx.ScenePaths.Clear();
            foreach (SceneAsset scene in ctx.Profile.scenes)
            {
                if (scene == null)
                {
                    ctx.Warn("The scene list contains an empty slot; it was skipped.");
                    continue;
                }
                string path = AssetDatabase.GetAssetPath(scene);
                if (string.IsNullOrEmpty(path))
                {
                    ctx.Warn($"Scene '{scene.name}' has no asset path and was skipped.");
                    continue;
                }
                ctx.ScenePaths.Add(path);
            }
            if (ctx.ScenePaths.Count == 0)
                throw new PS2BuildException("No usable scenes resolved from the profile.");

            Directory.CreateDirectory(ctx.ContentDirectory);

            // The content list is built HERE, not in the export step.
            //
            // Export is cacheable and skips itself when nothing changed, so a
            // list populated inside its Run() is empty on every incremental
            // build -- and Package then ships an ELF with no scene next to
            // it. The output paths are a pure function of the scene paths, so
            // deriving them in a step that always runs is both simpler and
            // correct in the cached case.
            ctx.ContentFiles.Clear();
            foreach (string scenePath in ctx.ScenePaths)
                ctx.ContentFiles.Add(PS2StepExportScenes.OutputFor(ctx, scenePath));
        }
    }

    /// <summary>
    /// 3 + 4. Export scenes and their assets. One step rather than the plan's
    /// two, because the exporter walks a scene and emits its meshes, textures,
    /// animation, audio and colliders into ONE .p2b container -- splitting the
    /// step would mean opening every scene twice.
    /// </summary>
    public sealed class PS2StepExportScenes : IPS2BuildStep
    {
        public string Name => "Export scenes and assets";

        public bool IsUpToDate(PS2BuildContext ctx)
        {
            if (ctx.ForceRebuild)
                return false;
            string hash = ComputeHash(ctx);
            if (hash != ctx.Cache.Get(Name))
                return false;
            // The cache can only be trusted if the outputs are actually there.
            foreach (string scene in ctx.ScenePaths)
            {
                if (!File.Exists(OutputFor(ctx, scene)))
                    return false;
            }
            return true;
        }

        public void Run(PS2BuildContext ctx)
        {
            string active = EditorSceneManager.GetActiveScene().path;
            // The profile's texture ceiling was validated but never applied,
            // so a build could warn about a 1024x1024 texture and then ship
            // it into VRAM that could not hold it (verify-log M12.5).
            P2bSceneExporter.MaxTextureSize = ctx.Profile.textureMaxSize;
            P2bSceneExporter.SkyboxFaceSize =
                ctx.Profile.exportSkybox ? ctx.Profile.skyboxFaceSize : 0;
            P2bSceneExporter.StaticBatching = ctx.Profile.staticBatching;
            P2bSceneExporter.StaticBatchCellSize = ctx.Profile.staticBatchCellSize;
            P2bTextureExporter.Mode =
                ctx.Profile.textureFormat == PS2TextureFormat.FourBit
                    ? P2bTextureExporter.FormatMode.FourBit
                    : ctx.Profile.textureFormat == PS2TextureFormat.EightBit
                        ? P2bTextureExporter.FormatMode.EightBit
                        : P2bTextureExporter.FormatMode.Auto;
            P2bSceneExporter.AudioSampleRate = ctx.Profile.audioSampleRate;
            foreach (string scenePath in ctx.ScenePaths)
            {
                string output = OutputFor(ctx, scenePath);
                EditorSceneManager.OpenScene(scenePath, OpenSceneMode.Single);
                P2bSceneExporter.LastStats = null;
                P2bSceneExporter.ExportActiveScene(output);
                if (!File.Exists(output))
                    throw new PS2BuildException(
                        $"Exporting '{scenePath}' produced no output at '{output}'.");
                // M14: the scene's budget, checked against the profile.
                PS2SceneBudget budget = P2bSceneExporter.LastStats;
                if (budget != null)
                {
                    budget.scene = Path.GetFileNameWithoutExtension(scenePath);
                    ctx.SceneBudgets.Add(budget);
                    if (budget.textureKb > ctx.Profile.textureBudgetKb)
                        ctx.Warn($"scene '{budget.scene}': textures need {budget.textureKb} KB of " +
                                 $"VRAM against the profile's {ctx.Profile.textureBudgetKb} KB " +
                                 "budget; some will be skipped at load. Largest: " +
                                 string.Join(", ", budget.largestTextures));
                    int tris = budget.triangles + budget.skinnedTriangles;
                    if (tris > ctx.Profile.triangleBudget)
                        ctx.Warn($"scene '{budget.scene}': {tris} triangles against the " +
                                 $"profile's {ctx.Profile.triangleBudget}; expect dropped " +
                                 "frames. Largest: " + string.Join(", ", budget.largestMeshes));
                    if (budget.drawCommands > 400)
                        ctx.Warn($"scene '{budget.scene}': {budget.drawCommands} draw commands " +
                                 "a frame; mark props Batching Static or reduce them.");
                }
            }
            // Put the Editor back where it was; a build must not quietly
            // change which scene the user has open.
            if (!string.IsNullOrEmpty(active) && File.Exists(active))
                EditorSceneManager.OpenScene(active, OpenSceneMode.Single);

            ctx.Cache.Set(Name, ComputeHash(ctx));
        }

        internal static string OutputFor(PS2BuildContext ctx, string scenePath)
        {
            return Path.Combine(ctx.ContentDirectory,
                                Path.GetFileNameWithoutExtension(scenePath) + ".p2b");
        }

        private static string ComputeHash(PS2BuildContext ctx)
        {
            // The scenes plus every asset they reference, so touching a mesh
            // invalidates the scene that uses it.
            var inputs = new List<string>();
            foreach (string scene in ctx.ScenePaths)
            {
                inputs.Add(scene);
                foreach (string dep in AssetDatabase.GetDependencies(scene, true))
                    inputs.Add(dep);
            }
            inputs.Sort(StringComparer.Ordinal);
            return PS2ContentHash.OfString(
                PS2ContentHash.OfFiles(inputs) +
                PS2ContentHash.OfProfile(ctx.Profile) +
                PS2ContentHash.OfBuildCode(ctx.PackageRoot));
        }
    }

    /// <summary>
    /// 5. Compile managed -- assemble the CLOSED assembly set IL2CPP will
    /// consume, then strip it with UnityLinker.
    ///
    /// The closure matters. il2cpp does NO reference probing (verify-log, M0):
    /// it resolves only what it is handed. Passing the game assembly alone
    /// fails with "netstandard was not resolved up front" -- an error that
    /// names a facade assembly nobody referenced on purpose. So the whole AOT
    /// BCL is staged alongside the game assembly and the shim, and il2cpp is
    /// pointed at the DIRECTORY rather than at a list of files.
    /// </summary>
    public sealed class PS2StepCompileManaged : IPS2BuildStep
    {
        public string Name => "Compile and strip managed";

        public bool IsUpToDate(PS2BuildContext ctx)
        {
            if (ctx.ForceRebuild)
                return false;
            return ComputeHash(ctx) == ctx.Cache.Get(Name) &&
                   Directory.Exists(StrippedDirectory(ctx)) &&
                   File.Exists(Path.Combine(StrippedDirectory(ctx), "Assembly-CSharp.dll"));
        }

        public void Run(PS2BuildContext ctx)
        {
            string managed = ManagedDirectory(ctx);
            if (Directory.Exists(managed))
                Directory.Delete(managed, true);
            Directory.CreateDirectory(managed);

            // The AOT BCL first, then the project's own assemblies on top so a
            // name clash resolves in favour of the game.
            foreach (string dll in Directory.GetFiles(ctx.Toolchain.UnityAotBclDir, "*.dll"))
                File.Copy(dll, Path.Combine(managed, Path.GetFileName(dll)), true);

            // The Facades subdirectory, which is where netstandard.dll lives.
            // unityaot-win32 itself does NOT contain it, and without it both
            // UnityLinker and il2cpp fail with "netstandard was not resolved"
            // on any assembly targeting netstandard2.x -- which the shim does.
            string facades = Path.Combine(ctx.Toolchain.UnityAotBclDir, "Facades");
            if (Directory.Exists(facades))
            {
                foreach (string dll in Directory.GetFiles(facades, "*.dll"))
                {
                    string destination = Path.Combine(managed, Path.GetFileName(dll));
                    if (!File.Exists(destination))
                        File.Copy(dll, destination);
                }
            }

            // The shim, which stands in for UnityEngine. Built on demand: it
            // is absent from every fresh clone (see BuildShimAssembly).
            string shim = FindShimAssembly(ctx) ?? BuildShimAssembly(ctx);
            File.Copy(shim, Path.Combine(managed, "PS2.UnityShim.dll"), true);

            // RECOMPILE the user's scripts against the shim rather than
            // reusing Unity's Assembly-CSharp.
            //
            // This is not an optimisation to skip. Unity's assembly references
            // UnityEngine.CoreModule and friends -- the real engine, none of
            // which exists on a PS2 -- so handing it to the linker fails with
            // "Failed to resolve assembly: UnityEngine.CoreModule". The shim
            // provides the same type names in the same namespaces, so the
            // SOURCE compiles against either; only the compiled references
            // differ. Plan 13.3 step 5 says "Roslyn recompile against the
            // shim" for exactly this reason.
            CompileGameAssembly(ctx, managed);

            string stripped = StrippedDirectory(ctx);
            if (Directory.Exists(stripped))
                Directory.Delete(stripped, true);

            string linkXml = WriteLinkXml(ctx);
            string linker = Path.Combine(
                Path.GetDirectoryName(ctx.Toolchain.Il2cppExe), "UnityLinker.exe");
            if (!File.Exists(linker))
                throw new PS2BuildException(
                    $"UnityLinker.exe was not found next to il2cpp at '{linker}'.");

            var args = new StringBuilder();
            args.Append($"--include-assembly=\"{Path.GetFullPath(Path.Combine(managed, "Assembly-CSharp.dll"))}\"");
            args.Append($" --include-link-xml=\"{linkXml}\"");
            args.Append($" --search-directory=\"{Path.GetFullPath(managed)}\"");
            args.Append($" --out=\"{Path.GetFullPath(stripped)}\"");
            args.Append(" --core-action=link");
            args.Append(" --i18n=none");
            args.Append($" --rule-set={RuleSet(ctx.Profile.strippingLevel)}");

            string output;
            int code = PS2Process.Run(linker, args.ToString(), ctx.ProjectRoot,
                                      out output, line => Debug.Log("[linker] " + line));
            if (code != 0)
                throw new PS2BuildException($"UnityLinker failed:\n{output}");

            ctx.Cache.Set(Name, ComputeHash(ctx));
        }

        internal static string ManagedDirectory(PS2BuildContext ctx) =>
            Path.Combine(ctx.IntermediateDirectory, "managed");

        internal static string StrippedDirectory(PS2BuildContext ctx) =>
            Path.Combine(ctx.IntermediateDirectory, "stripped");

        /// <summary>
        /// UnityLinker's --rule-set only accepts the names its own enum
        /// defines, and it fails hard on anything else ("Requested value
        /// 'balanced' was not found"). Only 'conservative' and 'aggressive'
        /// are used here because those are the two this project has actually
        /// run; the profile's four levels collapse onto them rather than
        /// inventing names that might not exist in a given Unity version.
        /// </summary>
        private static string RuleSet(PS2StrippingLevel level)
        {
            switch (level)
            {
                case PS2StrippingLevel.Disabled:
                case PS2StrippingLevel.Low:
                    return "conservative";
                default:
                    return "aggressive";
            }
        }

        /// <summary>
        /// link.xml preserving what reflection reaches. The dispatcher finds
        /// script types with Type.GetType and their lifecycle methods by
        /// reflection (M7), so without these the stripper removes the very
        /// code the game is made of.
        /// </summary>
        private static string WriteLinkXml(PS2BuildContext ctx)
        {
            string path = Path.Combine(ctx.IntermediateDirectory, "link.xml");
            var sb = new StringBuilder();
            sb.AppendLine("<linker>");
            sb.AppendLine("  <assembly fullname=\"Assembly-CSharp\" preserve=\"all\"/>");
            sb.AppendLine("  <assembly fullname=\"PS2.UnityShim\" preserve=\"all\"/>");
            sb.AppendLine("</linker>");
            File.WriteAllText(path, sb.ToString());

            // A user-supplied link.xml is merged by passing both to the
            // linker; here the profile's file is appended so its rules are not
            // lost.
            if (!string.IsNullOrEmpty(ctx.Profile.linkXmlPath) &&
                File.Exists(ctx.Profile.linkXmlPath))
            {
                string user = File.ReadAllText(ctx.Profile.linkXmlPath);
                string merged = sb.ToString().Replace("</linker>", "") +
                                user.Replace("<linker>", "").Replace("</linker>", "") +
                                "</linker>";
                File.WriteAllText(path, merged);
            }
            return Path.GetFullPath(path);
        }

        /// <summary>
        /// Compiles every game script against mscorlib + the shim, producing
        /// an Assembly-CSharp.dll whose references all exist on the console.
        /// </summary>
        private static void CompileGameAssembly(PS2BuildContext ctx, string managed)
        {
            List<string> sources = GameScripts(ctx.ProjectRoot);
            if (sources.Count == 0)
            {
                throw new PS2BuildException(
                    "No game scripts were found under Assets/. A build needs at " +
                    "least one MonoBehaviour to run.");
            }

            string cscExe, cscPrefix;
            if (!FindCsc(out cscExe, out cscPrefix))
                throw new PS2BuildException(
                    "No usable C# compiler was found. Install the .NET SDK (the " +
                    "build uses its Roslyn via 'dotnet exec csc.dll').");

            string responsePath = Path.Combine(ctx.IntermediateDirectory, "csc.rsp");
            var rsp = new StringBuilder();
            rsp.AppendLine("-target:library");
            rsp.AppendLine("-nostdlib+");
            rsp.AppendLine("-noconfig");
            rsp.AppendLine("-unsafe-");
            rsp.AppendLine($"-out:\"{Path.GetFullPath(Path.Combine(managed, "Assembly-CSharp.dll"))}\"");
            rsp.AppendLine($"-r:\"{Path.GetFullPath(Path.Combine(managed, "mscorlib.dll"))}\"");
            rsp.AppendLine($"-r:\"{Path.GetFullPath(Path.Combine(managed, "PS2.UnityShim.dll"))}\"");
            // The shim targets netstandard2.1, so its public signatures name
            // types from the netstandard facade. Without this reference every
            // use of a shim type fails with "The type 'Object' is defined in
            // an assembly that is not referenced".
            string netstandard = Path.Combine(managed, "netstandard.dll");
            if (File.Exists(netstandard))
                rsp.AppendLine($"-r:\"{Path.GetFullPath(netstandard)}\"");
            foreach (string define in ctx.Profile.scriptingDefines ?? new string[0])
            {
                if (!string.IsNullOrWhiteSpace(define))
                    rsp.AppendLine($"-define:{define.Trim()}");
            }
            foreach (string source in sources)
                rsp.AppendLine($"\"{Path.GetFullPath(source)}\"");
            File.WriteAllText(responsePath, rsp.ToString());

            string output;
            int code = PS2Process.Run(cscExe, $"{cscPrefix}@\"{responsePath}\"",
                                      ctx.ProjectRoot, out output,
                                      line => Debug.Log("[csc] " + line));
            if (code != 0)
            {
                throw new PS2BuildException(
                    "Compiling the game scripts against PS2.UnityShim failed. This " +
                    "usually means a script uses a Unity API the shim does not " +
                    "implement -- see docs/supported-api.md for the supported " +
                    $"subset.\n{output}");
            }
        }

        /// <summary>
        /// The user's runtime scripts: everything under Assets/ except Editor
        /// code, which never runs on the console.
        /// </summary>
        internal static List<string> GameScripts(string projectRoot)
        {
            var sources = new List<string>();
            string assets = Path.Combine(projectRoot, "Assets");
            if (!Directory.Exists(assets))
                return sources;
            foreach (string file in Directory.GetFiles(assets, "*.cs",
                                                       SearchOption.AllDirectories))
            {
                string normalised = file.Replace('\\', '/');
                if (normalised.Contains("/Editor/"))
                    continue;
                sources.Add(file);
            }
            sources.Sort(StringComparer.Ordinal); // deterministic argument order
            return sources;
        }

        /// <summary>
        /// Finds a C# compiler, preferring the .NET SDK's Roslyn run through
        /// 'dotnet exec'.
        ///
        /// Unity's bundled csc.exe is tempting -- it ships with the Editor, so
        /// it is always present -- but launching it directly fails with
        /// "Could not load file or assembly 'System.Text.Encoding.CodePages'":
        /// it expects to be started by Unity's own build host with that
        /// assembly resolvable. 'dotnet exec csc.dll' carries its own
        /// resolution and is what the M6/M7 build scripts have used all along.
        /// </summary>
        private static bool FindCsc(out string exe, out string prefixArgs)
        {
            exe = null;
            prefixArgs = "";

            string dotnetRoot = Environment.GetEnvironmentVariable("DOTNET_ROOT");
            var roots = new List<string>();
            if (!string.IsNullOrEmpty(dotnetRoot))
                roots.Add(Path.Combine(dotnetRoot, "sdk"));
            roots.Add("C:/Program Files/dotnet/sdk");
            roots.Add(Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                ".dotnet/sdk"));

            foreach (string root in roots)
            {
                if (!Directory.Exists(root))
                    continue;
                // Newest SDK last by ordinal sort, which is good enough: any
                // Roslyn recent enough to be in an SDK compiles what the shim
                // needs.
                foreach (string sdk in Directory.GetDirectories(root)
                                                .OrderByDescending(d => d, StringComparer.Ordinal))
                {
                    string csc = Path.Combine(sdk, "Roslyn/bincore/csc.dll");
                    if (!File.Exists(csc))
                        continue;
                    string dotnet = FindDotnetHost();
                    if (dotnet == null)
                        continue;
                    exe = dotnet;
                    prefixArgs = $"exec \"{csc}\" ";
                    return true;
                }
            }
            return false;
        }

        private static string FindDotnetHost()
        {
            string[] candidates =
            {
                "C:/Program Files/dotnet/dotnet.exe",
                Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                    ".dotnet/dotnet.exe"),
            };
            foreach (string candidate in candidates)
            {
                if (File.Exists(candidate))
                    return candidate;
            }
            return "dotnet"; // on PATH
        }

        private static string FindShimAssembly(PS2BuildContext ctx)
        {
            // Built output in this repository, then anywhere the project can
            // see it (a package would ship it prebuilt).
            var roots = new List<string>
            {
                Path.Combine(ctx.PackageRoot, "managed/PS2.UnityShim/bin/Release"),
                Path.Combine(ctx.PackageRoot, "managed/PS2.UnityShim/bin/Debug"),
                Path.Combine(ctx.ProjectRoot, "Assets"),
                Path.Combine(ctx.ProjectRoot, "Packages"),
                Path.Combine(ctx.ProjectRoot, "Library/ScriptAssemblies"),
            };
            foreach (string root in roots)
            {
                if (!Directory.Exists(root))
                    continue;
                string[] found = Directory.GetFiles(root, "PS2.UnityShim.dll",
                                                    SearchOption.AllDirectories);
                if (found.Length > 0)
                    return found[0];
            }
            return null;
        }

        /// <summary>
        /// Builds managed/PS2.Managed.sln because the shim is missing -- which
        /// is the state of every fresh clone or zip download: bin/ is
        /// gitignored, no .dll is tracked, and nothing ships it. Before this
        /// existed the first Build on a new machine failed with "PS2.UnityShim.dll
        /// was not found" and pointed at a dotnet command that only
        /// docs/development.md mentions. Runs once; afterwards FindShimAssembly
        /// finds the output and the hash in ComputeHash tracks it like any other
        /// input.
        ///
        /// Returns the built assembly's path. Throws with the compiler output
        /// on failure, and with the old guidance when there is no solution to
        /// build from (a prebuilt package must ship the DLL itself).
        /// </summary>
        private static string BuildShimAssembly(PS2BuildContext ctx)
        {
            string solution = Path.Combine(ctx.PackageRoot, "managed/PS2.Managed.sln");
            if (!File.Exists(solution))
                throw new PS2BuildException(
                    "PS2.UnityShim.dll was not found, and there is no " +
                    $"managed/PS2.Managed.sln under '{ctx.PackageRoot}' to build it " +
                    "from. A package distributed without the managed/ sources must " +
                    "ship the DLL prebuilt.");

            Debug.Log($"[PS2 Build] PS2.UnityShim.dll is not built yet (bin/ is never " +
                      $"committed). Building it once with 'dotnet build -c Release' from " +
                      $"{solution} ...");

            string output;
            int code;
            try
            {
                code = PS2Process.Run(FindDotnetHost(),
                                      $"build \"{solution}\" -c Release --nologo",
                                      ctx.PackageRoot, out output,
                                      line => Debug.Log("[dotnet] " + line));
            }
            catch (System.ComponentModel.Win32Exception e)
            {
                // Process.Start could not find 'dotnet' at all.
                throw new PS2BuildException(
                    "PS2.UnityShim.dll is not built and the .NET SDK is not installed, " +
                    $"so it cannot be built here ({e.Message}). Install the SDK from the " +
                    "README's requirements table (the dependency installer does this), " +
                    "then build again.");
            }
            if (code != 0)
                throw new PS2BuildException(
                    $"Building PS2.UnityShim failed (dotnet exit {code}). The build " +
                    $"output is above; the command was 'dotnet build \"{solution}\" " +
                    $"-c Release'.\n{output}");

            string shim = FindShimAssembly(ctx);
            if (shim == null)
                throw new PS2BuildException(
                    "dotnet build reported success but PS2.UnityShim.dll did not appear " +
                    $"under '{Path.Combine(ctx.PackageRoot, "managed/PS2.UnityShim/bin")}'.");
            Debug.Log($"[PS2 Build] Built {shim}");
            return shim;
        }

        private static string ComputeHash(PS2BuildContext ctx)
        {
            var inputs = GameScripts(ctx.ProjectRoot);
            string shim = FindShimAssembly(ctx);
            if (shim != null)
                inputs.Add(shim);
            return PS2ContentHash.OfString(
                PS2ContentHash.OfFiles(inputs) +
                PS2ContentHash.OfBuildCode(ctx.PackageRoot) +
                string.Join(",", ctx.Profile.scriptingDefines ?? new string[0]) +
                ctx.Profile.strippingLevel +
                (ctx.Profile.linkXmlPath ?? ""));
        }
    }

    /// <summary>6. Run IL2CPP -- the stripped assembly set to portable C++.</summary>
    public sealed class PS2StepRunIl2cpp : IPS2BuildStep
    {
        public string Name => "Run IL2CPP";

        public bool IsUpToDate(PS2BuildContext ctx)
        {
            if (ctx.ForceRebuild)
                return false;
            return ComputeHash(ctx) == ctx.Cache.Get(Name) &&
                   Directory.Exists(ctx.Il2cppOutputDirectory) &&
                   Directory.EnumerateFiles(ctx.Il2cppOutputDirectory, "*.cpp").Any();
        }

        public void Run(PS2BuildContext ctx)
        {
            string stripped = PS2StepCompileManaged.StrippedDirectory(ctx);
            if (!Directory.Exists(stripped))
                throw new PS2BuildException(
                    "The stripped assembly set is missing; the Compile and strip " +
                    "managed step must run before IL2CPP.");

            if (Directory.Exists(ctx.Il2cppOutputDirectory))
                Directory.Delete(ctx.Il2cppOutputDirectory, true);
            Directory.CreateDirectory(ctx.Il2cppOutputDirectory);

            // --dotnetprofile must be FULLY qualified: bare 'unityaot' is
            // rejected, and the switch is hidden from --help (verify-log, M0).
            // --directory hands il2cpp the whole closed set, which is what
            // avoids its lack of reference probing.
            var args = new StringBuilder();
            args.Append("--convert-to-cpp");
            args.Append($" --directory=\"{Path.GetFullPath(stripped)}\"");
            args.Append($" --generatedcppdir=\"{Path.GetFullPath(ctx.Il2cppOutputDirectory)}\"");
            args.Append(" --dotnetprofile=unityaot-win32");
            if (ctx.Profile.developmentBuild)
            {
                args.Append(" --emit-null-checks=true");
                args.Append(" --enable-array-bounds-check=true");
            }
            else
            {
                // The checks cost real cycles on a 300 MHz in-order CPU, and a
                // shipping build has been through the development one.
                args.Append(" --emit-null-checks=false");
                args.Append(" --enable-array-bounds-check=false");
                args.Append(" --enable-divide-by-zero-check=false");
            }
            foreach (string extra in ctx.Profile.additionalIl2cppArgs ?? new string[0])
            {
                if (!string.IsNullOrWhiteSpace(extra))
                    args.Append(" ").Append(extra);
            }

            string output;
            int code = PS2Process.Run(ctx.Toolchain.Il2cppExe, args.ToString(),
                                      ctx.ProjectRoot, out output,
                                      line => Debug.Log("[il2cpp] " + line));
            if (code != 0)
                throw new PS2BuildException(
                    $"il2cpp failed with exit code {code}.\n{output}");

            ctx.Cache.Set(Name, ComputeHash(ctx));
        }

        private static string ComputeHash(PS2BuildContext ctx)
        {
            string stripped = PS2StepCompileManaged.StrippedDirectory(ctx);
            var inputs = Directory.Exists(stripped)
                             ? Directory.GetFiles(stripped, "*.dll").OrderBy(
                                   p => p, StringComparer.Ordinal).ToList()
                             : new List<string>();
            return PS2ContentHash.OfString(
                PS2ContentHash.OfFiles(inputs) +
                PS2ContentHash.OfBuildCode(ctx.PackageRoot) +
                string.Join(",", ctx.Profile.additionalIl2cppArgs ?? new string[0]) +
                ctx.Profile.developmentBuild);
        }
    }

    /// <summary>7. Generate bridge -- bindgen over bridge-api.json.</summary>
    public sealed class PS2StepGenerateBridge : IPS2BuildStep
    {
        public string Name => "Generate bridge";

        public bool IsUpToDate(PS2BuildContext ctx)
        {
            // bindgen has its own freshness check and it is a second to run,
            // so this always runs and simply verifies. Caching a code
            // generator saves nothing and risks shipping a stale boundary,
            // which is a days-to-find class of bug on hardware.
            return false;
        }

        public void Run(PS2BuildContext ctx)
        {
            string bindgen = Path.Combine(ctx.PackageRoot, "tools/bindgen/bindgen.py");
            if (!File.Exists(bindgen))
            {
                ctx.Warn("bindgen was not found; the bridge was not regenerated.");
                return;
            }
            string output;
            int code = PS2Process.Run(ctx.Toolchain.PythonExe,
                                      $"\"{bindgen}\" gen-all", ctx.PackageRoot,
                                      out output);
            if (code != 0)
                throw new PS2BuildException($"bindgen failed:\n{output}");
        }
    }

    /// <summary>8. Native build -- CMake + Ninja with the PS2 toolchain file.</summary>
    public sealed class PS2StepNativeBuild : IPS2BuildStep
    {
        public string Name => "Native build";

        public bool IsUpToDate(PS2BuildContext ctx)
        {
            // Never: Ninja's own dependency tracking is finer-grained than
            // anything this step could compute, and re-running it on an
            // up-to-date tree costs a fraction of a second (plan 13.3 task 6).
            return false;
        }

        public void Run(PS2BuildContext ctx)
        {
            string toolchainFile =
                Path.Combine(ctx.PackageRoot, "tools/cmake/ps2-toolchain.cmake");
            if (!File.Exists(toolchainFile))
                throw new PS2BuildException(
                    $"The PS2 CMake toolchain file is missing at '{toolchainFile}'.");

            Directory.CreateDirectory(ctx.NativeBuildDirectory);

            var configure = new StringBuilder();
            configure.Append($"-S \"{ctx.PackageRoot}\"");
            configure.Append($" -B \"{Path.GetFullPath(ctx.NativeBuildDirectory)}\"");
            if (ctx.Toolchain.NinjaExe != null)
                configure.Append(" -G Ninja");
            configure.Append($" -DCMAKE_TOOLCHAIN_FILE=\"{toolchainFile}\"");
            configure.Append(" -DPS2UR_PLATFORM=ps2");
            configure.Append(" -DPS2UR_BUILD_TESTS=OFF");
            configure.Append(ctx.Profile.optimisation == PS2Optimisation.Debug
                                 ? " -DCMAKE_BUILD_TYPE=Debug"
                                 : " -DCMAKE_BUILD_TYPE=Release");

            string output;
            int code = PS2Process.Run(ctx.Toolchain.CMakeExe, configure.ToString(),
                                      ctx.PackageRoot, out output,
                                      line => Debug.Log("[cmake] " + line));
            if (code != 0)
                throw new PS2BuildException($"CMake configure failed:\n{output}");

            code = PS2Process.Run(
                ctx.Toolchain.CMakeExe,
                $"--build \"{Path.GetFullPath(ctx.NativeBuildDirectory)}\"",
                ctx.PackageRoot, out output, line => Debug.Log("[build] " + line));
            if (code != 0)
                throw new PS2BuildException($"Native build failed:\n{output}");

            BuildGameExecutable(ctx, toolchainFile);
        }

        /// <summary>
        /// Links the IL2CPP output, libil2cpp and the runtime into the game
        /// ELF.
        ///
        /// This is a SECOND CMake tree (il2cpp-port/) rather than a target in
        /// the first, because it compiles a patched copy of Unity's libil2cpp
        /// that must never be committed (plan section 17) and is staged into
        /// build/il2cpp by il2cpp-port/apply.py. Keeping it separate is what
        /// stops that copy leaking into the runtime's own build.
        /// </summary>
        private static void BuildGameExecutable(PS2BuildContext ctx, string toolchainFile)
        {
            string portRoot = Path.Combine(ctx.PackageRoot, "il2cpp-port");
            string staged = Path.Combine(ctx.PackageRoot, "build/il2cpp/libil2cpp");
            if (!Directory.Exists(portRoot))
            {
                // Fatal, not a warning. The game ELF is the build's entire
                // product; a step that reports success while linking nothing
                // sends the developer to read the wrong log.
                throw new PS2BuildException(
                    $"il2cpp-port/ is missing under '{ctx.PackageRoot}', so the game " +
                    "executable cannot be linked. It is part of the repository; a " +
                    "package distributed without it cannot produce an ELF.");
            }
            if (!Directory.Exists(staged))
                StageLibIl2cpp(ctx, portRoot, staged);

            string gameBuild = Path.Combine(ctx.IntermediateDirectory, "game");
            Directory.CreateDirectory(gameBuild);
            WriteGameConfig(ctx, gameBuild);

            var configure = new StringBuilder();
            configure.Append($"-S \"{portRoot}\"");
            configure.Append($" -B \"{Path.GetFullPath(gameBuild)}\"");
            if (ctx.Toolchain.NinjaExe != null)
                configure.Append(" -G Ninja");
            configure.Append($" -DCMAKE_TOOLCHAIN_FILE=\"{toolchainFile}\"");
            configure.Append($" -DM6_GENERATED=\"{Path.GetFullPath(ctx.Il2cppOutputDirectory)}\"");
            configure.Append($" -DPS2UR_RUNTIME=\"{Path.GetFullPath(ctx.NativeBuildDirectory)}/runtime\"");
            configure.Append(" -DM6_MAIN=main_game.cpp");
            configure.Append($" -DPS2_GAME_CONFIG_DIR=\"{Path.GetFullPath(gameBuild)}\"");
            configure.Append(" -DCMAKE_BUILD_TYPE=Release");
            // Always stated, never omitted: the CMake cache remembers the
            // last value, and a ladder build that outlived its profile
            // toggle would be a game that takes a minute to boot for no
            // visible reason.
            configure.Append(ctx.Profile.bootLadder ? " -DPS2_BOOT_LADDER=ON"
                                                    : " -DPS2_BOOT_LADDER=OFF");

            string output;
            int code = PS2Process.Run(ctx.Toolchain.CMakeExe, configure.ToString(),
                                      ctx.PackageRoot, out output,
                                      line => Debug.Log("[game-cmake] " + line));
            // A failure here is FATAL, not a warning.
            //
            // These two used to warn and return, from when the game host was
            // an experiment bolted onto a runtime-only build. It is now the
            // build's entire product, and warning meant a compile error came
            // back as "succeeded: true" with an empty elfPath -- a green
            // build that shipped nothing. Whatever else this pipeline gets
            // wrong, it must never report success for output it did not
            // produce.
            if (code != 0)
            {
                throw new PS2BuildException(
                    "The game executable could not be configured:\n" + output);
            }

            code = PS2Process.Run(ctx.Toolchain.CMakeExe,
                                  $"--build \"{Path.GetFullPath(gameBuild)}\"",
                                  ctx.PackageRoot, out output,
                                  line => Debug.Log("[game] " + line));
            if (code != 0)
            {
                throw new PS2BuildException(
                    "The game executable failed to build:\n" + output);
            }
            ctx.GameBuildDirectory = gameBuild;
        }

        /// <summary>
        /// Stages Unity's libil2cpp, patched for the PS2, under build/il2cpp by
        /// running il2cpp-port/apply.py prepare. Every fresh clone needs this
        /// once: the sources are Unity's and are deliberately never committed
        /// (plan section 17), so nothing else puts them there. This used to be
        /// a warning-and-return that reported success for a build which had
        /// linked nothing, pointing at a command whose default Unity root is
        /// hardcoded to one Editor version. The running Editor's own install
        /// is passed instead: by definition the version that generated the
        /// C++ about to be linked.
        /// </summary>
        private static void StageLibIl2cpp(PS2BuildContext ctx, string portRoot, string staged)
        {
            string script = Path.Combine(portRoot, "apply.py");
            if (!File.Exists(script))
                throw new PS2BuildException($"'{script}' is missing; libil2cpp cannot be staged.");
            if (string.IsNullOrEmpty(ctx.Toolchain.PythonExe))
                throw new PS2BuildException(
                    "python was not found on PATH, so il2cpp-port/apply.py cannot stage " +
                    "libil2cpp. Install Python (the dependency installer does this).");

            string unityRoot = EditorApplication.applicationContentsPath;
            string buildDir = Path.Combine(ctx.PackageRoot, "build/il2cpp");
            Debug.Log("[PS2 Build] libil2cpp is not staged under build/il2cpp (Unity source, " +
                      $"never committed). Staging it once from {unityRoot} with apply.py prepare ...");

            string output;
            int code = PS2Process.Run(
                ctx.Toolchain.PythonExe,
                $"\"{script}\" prepare --unity-root \"{unityRoot}\" --build-dir \"{buildDir}\"",
                ctx.PackageRoot, out output, line => Debug.Log("[apply.py] " + line));
            if (code != 0)
                throw new PS2BuildException(
                    $"Staging libil2cpp failed (apply.py exit {code}). The command was " +
                    $"'python il2cpp-port/apply.py prepare --unity-root \"{unityRoot}\"'.\n{output}");
            if (!Directory.Exists(staged))
                throw new PS2BuildException(
                    $"apply.py reported success but '{staged}' does not exist afterwards.");
            Debug.Log($"[PS2 Build] Staged libil2cpp at {staged}");
        }

        /// <summary>
        /// Everything scene- and profile-specific the game host needs, as a
        /// generated header.
        ///
        /// A header rather than compiler -D flags because the boot scene name
        /// is a string: getting a quoted string through CMake, Ninja and a
        /// shell intact is a portability problem nobody needs, and a header is
        /// also something a developer can read to see exactly what the build
        /// decided.
        /// </summary>
        private static void WriteGameConfig(PS2BuildContext ctx, string gameBuild)
        {
            PS2BuildProfile p = ctx.Profile;
            string bootScene = ctx.ContentFiles.Count > 0
                                   ? Path.GetFileName(ctx.ContentFiles[0])
                                   : Path.GetFileNameWithoutExtension(
                                         ctx.ScenePaths.Count > 0 ? ctx.ScenePaths[0] : "scene") +
                                     ".p2b";

            var sb = new StringBuilder();
            sb.AppendLine("// GENERATED by the PS2 build pipeline. Do not edit:");
            sb.AppendLine("// every value here comes from the build profile, and");
            sb.AppendLine("// this file is rewritten on every build.");
            sb.AppendLine("#pragma once");
            sb.AppendLine();
            sb.AppendLine($"#define PS2_GAME_PRODUCT_NAME \"{Escape(p.productName)}\"");
            sb.AppendLine($"#define PS2_GAME_BOOT_SCENE \"{Escape(bootScene)}\"");
            // PlayerPrefs' on-card home (M12.5 task 3): the browser
            // convention is a directory named by the disc serial.
            sb.AppendLine($"#define PS2_GAME_SAVE_DIRECTORY \"{Escape(p.discSerial)}\"");
            sb.AppendLine($"#define PS2_GAME_SCREEN_WIDTH {p.FramebufferWidth}");
            sb.AppendLine($"#define PS2_GAME_SCREEN_HEIGHT {p.FramebufferHeight}");
            sb.AppendLine($"#define PS2_GAME_ASSET_POOL_BYTES ({p.assetPoolMb} * 1024 * 1024)");
            sb.AppendLine($"#define PS2_GAME_MANAGED_HEAP_BYTES ({p.managedHeapMb} * 1024 * 1024)");
            sb.AppendLine($"#define PS2_GAME_DEVELOPMENT {(p.developmentBuild ? 1 : 0)}");
            // Which media the game reads (profile: Host Filesystem). 0 means
            // host: is never tried, so the game reads exactly what a console
            // reads and the ISO must be self-contained; 1 tries host: first
            // and falls back to the disc.
            sb.AppendLine($"#define PS2_GAME_HOST_FS {(p.hostFilesystem ? 1 : 0)}");
            // The frame at which the host prints its liveness token. Late
            // enough that a scene which crashes on frame 2 does not look
            // healthy, early enough that a test does not wait seconds for it.
            sb.AppendLine("#define PS2_GAME_READY_FRAME 30");
            // Profiler (M13). The overlay is always compiled in and starts
            // off; the profiler flag decides whether the build ALSO measures
            // a fixed window and dumps it for CI. A frame count rather than
            // a timer keeps the measurement reproducible across runs.
            sb.AppendLine($"#define PS2_GAME_PROFILE_FRAMES {(p.profiler ? 600 : 0)}");
            File.WriteAllText(Path.Combine(gameBuild, "game_config.h"), sb.ToString());
        }

        private static string Escape(string value) =>
            (value ?? "").Replace("\\", "\\\\").Replace("\"", "\\\"");
    }

    /// <summary>
    /// 9. Package -- SYSTEM.CNF, the mkps2iso script in profiled LBA order,
    /// and the ISO itself.
    /// </summary>
    public sealed class PS2StepPackage : IPS2BuildStep
    {
        public string Name => "Package";

        public bool IsUpToDate(PS2BuildContext ctx) => false;

        public void Run(PS2BuildContext ctx)
        {
            Directory.CreateDirectory(ctx.OutputDirectory);

            // The staging directory IS the disc's root, so what goes in it is
            // exactly what ships.
            string stage = Path.Combine(ctx.IntermediateDirectory, "iso");
            if (Directory.Exists(stage))
                Directory.Delete(stage, true);
            Directory.CreateDirectory(stage);

            string elf = FindBuiltElf(ctx);
            if (elf == null)
            {
                throw new PS2BuildException(
                    "No game ELF was produced, so there is nothing to package. " +
                    "The native build step should already have failed; if the " +
                    "build got this far, look at its log rather than at this " +
                    "message.");
            }
            File.Copy(elf, ctx.ElfPath, true);
            File.Copy(elf, Path.Combine(stage, ctx.Profile.bootElfName), true);

            // The content goes to BOTH the ISO staging directory and next to
            // game.elf.
            //
            // Next to the ELF matters more than it looks: PCSX2 derives the
            // host: root from the ELF's own directory, so an ELF sitting
            // alone in the output folder resolves host:scene.p2b to a file
            // that is not there, reports the scene missing and parks. The
            // symptom is a black screen with a frozen FPS counter -- which
            // reads exactly like a crash, and is not one.
            foreach (string content in ctx.ContentFiles)
            {
                if (!File.Exists(content))
                    continue;
                string name = Path.GetFileName(content);
                File.Copy(content, Path.Combine(stage, name), true);
                File.Copy(content, Path.Combine(ctx.OutputDirectory, name), true);
            }

            // IOP modules. The runtime tries host:, mass: and cdrom0: before
            // falling back to a blob embedded in the ELF, and the fallback
            // does not reliably START every module (verify-log M10:
            // SifExecModuleBuffer silently fails for some). Shipping the real
            // files means the first, reliable path is the one taken -- the
            // symptom otherwise is a boot that stops dead after
            // "loadmodule: id -203" with no error.
            string irxDir = Path.Combine(ctx.Toolchain.Ps2SdkDir, "iop/irx");
            // These names must match what platform::load_irx asks for at
            // boot. The list once said "freemcman.irx"/"freemcserv.irx" --
            // names this sdk does not ship -- and the loop below skipped
            // them WITHOUT A WORD, so PlayerPrefs booted into "loadmodule:
            // id -203" on a build every step of which reported success
            // (verify-log M12.5).
            string[] modules =
            {
                "freesio2.irx", "freepad.irx", "mcman.irx", "mcserv.irx",
                "audsrv.irx", "libsd.irx", "iomanX.irx", "fileXio.irx",
            };
            foreach (string module in modules)
            {
                string source = Path.Combine(irxDir, module);
                if (!File.Exists(source))
                {
                    ctx.Warn(
                        $"IOP module '{module}' is not in '{irxDir}', so it " +
                        "was not packaged. Whatever runtime feature loads it " +
                        "will fail at boot with 'loadmodule: id -203'.");
                    continue;
                }
                File.Copy(source, Path.Combine(stage, module), true);
                File.Copy(source, Path.Combine(ctx.OutputDirectory, module), true);
            }

            // il2cpp's global-metadata.dat, which the managed runtime opens
            // through the same host: root. Without it il2cpp_init fails after
            // the scene has already loaded, which is a confusing place to
            // find out the build was incomplete.
            string metadataSource = Path.Combine(ctx.Il2cppOutputDirectory,
                                                 "Data", "Metadata",
                                                 "global-metadata.dat");
            if (File.Exists(metadataSource))
            {
                foreach (string root in new[] { stage, ctx.OutputDirectory })
                {
                    string dir = Path.Combine(root, "Metadata");
                    Directory.CreateDirectory(dir);
                    File.Copy(metadataSource,
                              Path.Combine(dir, "global-metadata.dat"), true);
                }
            }
            else
            {
                ctx.Warn(
                    "global-metadata.dat was not found in the IL2CPP output, so " +
                    "the managed runtime will fail to start. Re-run the build " +
                    "with Force Rebuild.");
            }

            string systemCnf = Path.Combine(stage, "SYSTEM.CNF");
            File.WriteAllText(systemCnf, BuildSystemCnf(ctx.Profile));

            if (!ctx.Profile.buildIso)
                return;
            if (ctx.Toolchain.MkPs2IsoExe == null)
            {
                ctx.Warn("mkps2iso is not installed, so game.iso was not produced. " +
                         "game.elf is still runnable over host: in PCSX2.");
                return;
            }

            // Delete the previous image OURSELVES, for two reasons that
            // compounded in the field (verify-log M12.5): mkps2iso asks
            // "overwrite? <Y/n>" on an existing file, a question nothing can
            // answer in a headless pipeline -- and when PCSX2 is still
            // running with the previous build's ISO mounted, Windows holds a
            // lock and the write fails with a message that names neither
            // cause. Deleting first turns both into one actionable error.
            string isoPath = ctx.IsoPath;
            if (File.Exists(isoPath))
            {
                try
                {
                    File.Delete(isoPath);
                }
                catch (IOException)
                {
                    throw new PS2BuildException(
                        $"'{isoPath}' is open in another program, almost " +
                        "certainly PCSX2 with the previous build's disc still " +
                        "mounted. Close PCSX2 (or eject the disc in it) and " +
                        "build again.");
                }
            }

            string script = Path.Combine(ctx.IntermediateDirectory, "disc.xml");
            WriteIsoScript(ctx, stage, script);

            string output;
            int code = PS2Process.Run(ctx.Toolchain.MkPs2IsoExe, $"\"{script}\"",
                                      ctx.IntermediateDirectory, out output,
                                      line => Debug.Log("[mkps2iso] " + line));
            if (code != 0)
                throw new PS2BuildException($"mkps2iso failed:\n{output}");
        }

        /// <summary>
        /// SYSTEM.CNF is what the BIOS reads to find the executable. The
        /// backslash and the ';1' version suffix are both required -- this is
        /// ISO 9660, not a POSIX path -- and a PAL console additionally
        /// insists the file name match the serial.
        /// </summary>
        internal static string BuildSystemCnf(PS2BuildProfile profile)
        {
            var sb = new StringBuilder();
            sb.Append(profile.Boot2Line).Append("\r\n");
            sb.Append("VER = ").Append(profile.version).Append("\r\n");
            sb.Append("VMODE = ").Append(profile.VideoModeToken).Append("\r\n");
            return sb.ToString();
        }

        private void WriteIsoScript(PS2BuildContext ctx, string stage, string scriptPath)
        {
            // File ORDER is LBA order. Where a profiled first-access trace is
            // available the planner produces the order that makes the drive
            // read forward (M10 task 4); without one it is boot-first then
            // alphabetical, which at least keeps the executable at the front.
            List<string> order = PlanOrder(ctx, stage);

            var sb = new StringBuilder();
            sb.AppendLine("<!-- Generated by the PS2 build pipeline (plan 13.3 step 9). -->");
            sb.AppendLine($"<iso_project image_name=\"{Path.GetFullPath(ctx.IsoPath)}\" " +
                          $"serial=\"{ctx.Profile.discSerial}\" " +
                          $"region=\"{(ctx.Profile.region == PS2Region.PAL ? "europe" : "america")}\">");
            sb.AppendLine("    <identifiers");
            sb.AppendLine("        system          =\"PLAYSTATION\"");
            sb.AppendLine("        application     =\"PLAYSTATION\"");
            sb.AppendLine($"        volume          =\"{ctx.Profile.volumeLabel}\"");
            sb.AppendLine("        data_preparer   =\"unity-ps2\"");
            sb.AppendLine("    />");
            sb.AppendLine("    <layer>");
            sb.AppendLine($"        <directory_tree source=\"{Path.GetFullPath(stage)}\">");
            foreach (string file in order)
                sb.AppendLine($"            <file name=\"{file}\"/>");
            // il2cpp's metadata lives in a subdirectory, which PlanOrder's
            // top-level listing never saw -- so no ISO this pipeline made
            // could start the managed runtime without PCSX2's host: pointing
            // at the build folder. Always on the disc: a host:-off build has
            // nowhere else to read it from, and a host:-on build still boots
            // from the disc alone when the emulator is not serving host:.
            if (File.Exists(Path.Combine(stage, "Metadata", "global-metadata.dat")))
            {
                sb.AppendLine("            <dir name=\"Metadata\">");
                sb.AppendLine("                <file name=\"global-metadata.dat\"/>");
                sb.AppendLine("            </dir>");
            }
            sb.AppendLine("        </directory_tree>");
            sb.AppendLine("    </layer>");
            sb.AppendLine("</iso_project>");
            File.WriteAllText(scriptPath, sb.ToString());
        }

        private List<string> PlanOrder(PS2BuildContext ctx, string stage)
        {
            var staged = Directory.GetFiles(stage)
                                  .Select(Path.GetFileName)
                                  .OrderBy(n => n, StringComparer.Ordinal)
                                  .ToList();
            var order = new List<string>();

            // SYSTEM.CNF must be first: it is the first thing the BIOS reads.
            if (staged.Remove("SYSTEM.CNF"))
                order.Add("SYSTEM.CNF");
            if (staged.Remove(ctx.Profile.bootElfName))
                order.Add(ctx.Profile.bootElfName);

            string trace = ctx.Profile.discTracePath;
            if (!string.IsNullOrEmpty(trace) && File.Exists(trace))
            {
                foreach (string line in File.ReadAllLines(trace))
                {
                    int marker = line.IndexOf("M10_TRACE", StringComparison.Ordinal);
                    if (marker < 0)
                        continue;
                    string[] parts = line.Substring(marker).Split(
                        new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
                    if (parts.Length < 3)
                        continue;
                    string name = parts[2];
                    int colon = name.IndexOf(':');
                    if (colon >= 0)
                        name = name.Substring(colon + 1);
                    name = name.Replace('\\', '/');
                    name = name.Substring(name.LastIndexOf('/') + 1);
                    string match = staged.FirstOrDefault(
                        s => string.Equals(s, name, StringComparison.OrdinalIgnoreCase));
                    if (match != null)
                    {
                        staged.Remove(match);
                        order.Add(match);
                    }
                }
            }
            order.AddRange(staged);
            return order;
        }

        private static string FindBuiltElf(PS2BuildContext ctx)
        {
            // The game tree first: that is the ELF that hosts managed code.
            // The runtime tree only ever contains samples.
            if (!string.IsNullOrEmpty(ctx.GameBuildDirectory) &&
                Directory.Exists(ctx.GameBuildDirectory))
            {
                string[] game = Directory
                    .GetFiles(ctx.GameBuildDirectory, "*.elf", SearchOption.AllDirectories)
                    .OrderBy(p => p, StringComparer.Ordinal)
                    .ToArray();
                if (game.Length > 0)
                    return game[0];
            }
            if (!Directory.Exists(ctx.NativeBuildDirectory))
                return null;
            // The game ELF, if the native tree defines one. Samples are
            // ignored: they are not the product.
            string[] candidates = Directory
                .GetFiles(ctx.NativeBuildDirectory, "*.elf", SearchOption.AllDirectories)
                .Where(p => !p.Replace('\\', '/').Contains("/samples/"))
                .OrderBy(p => p, StringComparer.Ordinal)
                .ToArray();
            return candidates.Length > 0 ? candidates[0] : null;
        }
    }

    /// <summary>10. Deploy -- PCSX2, ps2client, or a USB folder.</summary>
    public sealed class PS2StepDeploy : IPS2BuildStep
    {
        public string Name => "Deploy";

        public bool IsUpToDate(PS2BuildContext ctx) => false;

        public void Run(PS2BuildContext ctx)
        {
            switch (ctx.Profile.deployTarget)
            {
                case PS2DeployTarget.None:
                    return;
                case PS2DeployTarget.UsbFolder:
                    DeployToFolder(ctx);
                    return;
                case PS2DeployTarget.Ps2Client:
                    DeployToConsole(ctx);
                    return;
                default:
                    LaunchEmulator(ctx);
                    return;
            }
        }

        private static void DeployToFolder(PS2BuildContext ctx)
        {
            if (string.IsNullOrEmpty(ctx.Profile.usbFolder))
                throw new PS2BuildException(
                    "Deploy target is USB folder but no folder is set on the profile.");
            Directory.CreateDirectory(ctx.Profile.usbFolder);
            string target = Path.Combine(ctx.Profile.usbFolder, ctx.Profile.bootElfName);
            File.Copy(ctx.ElfPath, target, true);
            Debug.Log($"[PS2 Build] copied to {target} -- launch it with wLaunchELF.");
        }

        private static void DeployToConsole(PS2BuildContext ctx)
        {
            if (ctx.Toolchain.Ps2ClientExe == null)
                throw new PS2BuildException(
                    "Deploy target is ps2client but ps2client was not found on PATH.");
            string output;
            int code = PS2Process.Run(
                ctx.Toolchain.Ps2ClientExe,
                $"-h {ctx.Profile.ps2ClientHost} execee host:{Path.GetFileName(ctx.ElfPath)}",
                ctx.OutputDirectory, out output, line => Debug.Log("[ps2client] " + line));
            if (code != 0)
                throw new PS2BuildException($"ps2client failed:\n{output}");
        }

        private static void LaunchEmulator(PS2BuildContext ctx)
        {
            string pcsx2 = !string.IsNullOrEmpty(ctx.Profile.pcsx2Path)
                               ? ctx.Profile.pcsx2Path
                               : ctx.Toolchain.Pcsx2Exe;
            if (string.IsNullOrEmpty(pcsx2) || !File.Exists(pcsx2))
                throw new PS2BuildException(
                    "PCSX2 was not found. Set its path on the build profile, or " +
                    "install it where the toolchain locator looks.");

            // PCSX2 refuses a path containing a space and executes garbage from
            // a relative one (verify-log, M0). Both are avoided by passing an
            // absolute path and, when it contains a space, staging a copy.
            string target = ctx.Profile.buildIso && File.Exists(ctx.IsoPath)
                                ? Path.GetFullPath(ctx.IsoPath)
                                : Path.GetFullPath(ctx.ElfPath);
            if (target.Contains(" "))
            {
                string stage = Path.Combine(Path.GetTempPath(), "ps2-build-run");
                Directory.CreateDirectory(stage);
                string staged = Path.Combine(stage, Path.GetFileName(target));
                File.Copy(target, staged, true);
                Debug.Log($"[PS2 Build] output path contains a space; running the " +
                          $"staged copy at {staged}");
                target = staged;
            }

            // PCSX2 serves host: only with [EmuCore] HostFs on in its ini, and
            // the default is off. A profile that reads over host: needs it;
            // without it the boot died at magenta with the reason sitting in
            // an EE console nobody had enabled. A host:-off build is left
            // alone: it must boot from the disc whatever PCSX2 is set to.
            if (ctx.Profile.hostFilesystem)
                EnsurePcsx2HostFs(pcsx2);

            // A console build boots the way a console does: through the
            // BIOS, which reads SYSTEM.CNF and the boot ELF the way the
            // silicon will (-slowboot). A host: build is an iteration loop
            // and skips that (-fastboot). Either flag overrides PCSX2's own
            // Fast Boot setting, so the profile decides, not the ini.
            string boot = ctx.Profile.hostFilesystem ? "-fastboot" : "-slowboot";
            string args = target.EndsWith(".iso", StringComparison.OrdinalIgnoreCase)
                              ? $"-batch {boot} -- \"{target}\""
                              : $"-batch {boot} -elf \"{target}\"";
            System.Diagnostics.Process.Start(
                new System.Diagnostics.ProcessStartInfo(pcsx2, args)
                {
                    UseShellExecute = false,
                    WorkingDirectory = Path.GetDirectoryName(pcsx2),
                });
            Debug.Log($"[PS2 Build] launched PCSX2 with {target}");
        }

        /// <summary>
        /// Turns on [EmuCore] HostFs in PCSX2's ini if it is off. The ini is
        /// next to the executable for a portable install, otherwise under the
        /// user's Documents. Anything unexpected is logged and skipped: a
        /// missing ini is PCSX2's first-run state, not a build failure.
        /// </summary>
        private static void EnsurePcsx2HostFs(string pcsx2Exe)
        {
            string exeDir = Path.GetDirectoryName(pcsx2Exe) ?? "";
            bool portable = File.Exists(Path.Combine(exeDir, "portable.ini")) ||
                            File.Exists(Path.Combine(exeDir, "portable.txt"));
            string ini = portable
                ? Path.Combine(exeDir, "inis", "PCSX2.ini")
                : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
                               "PCSX2", "inis", "PCSX2.ini");
            if (!File.Exists(ini))
            {
                Debug.Log($"[PS2 Build] PCSX2.ini not found at {ini}; if the game stops on " +
                          "magenta, enable the host filesystem in PCSX2's settings.");
                return;
            }

            string[] lines = File.ReadAllLines(ini);
            int section = Array.FindIndex(lines, l => l.Trim() == "[EmuCore]");
            if (section < 0)
            {
                Debug.Log($"[PS2 Build] no [EmuCore] section in {ini}; leaving it alone.");
                return;
            }
            int end = section + 1;
            while (end < lines.Length && !lines[end].TrimStart().StartsWith("["))
                end++;
            for (int i = section + 1; i < end; i++)
            {
                string t = lines[i].Trim();
                if (!t.StartsWith("HostFs", StringComparison.OrdinalIgnoreCase))
                    continue;
                if (t.EndsWith("true", StringComparison.OrdinalIgnoreCase))
                    return; // already on
                lines[i] = "HostFs = true";
                File.WriteAllLines(ini, lines);
                Debug.Log("[PS2 Build] enabled PCSX2's host filesystem ([EmuCore] HostFs) so " +
                          "this host: build can read the build folder.");
                return;
            }
            var withKey = new List<string>(lines);
            withKey.Insert(section + 1, "HostFs = true");
            File.WriteAllLines(ini, withKey);
            Debug.Log("[PS2 Build] enabled PCSX2's host filesystem ([EmuCore] HostFs) so " +
                      "this host: build can read the build folder.");
        }
    }
}
