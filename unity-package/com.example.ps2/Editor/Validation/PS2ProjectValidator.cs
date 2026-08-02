using System.Collections.Generic;
using System.IO;
using UnityEditor;

namespace Ps2.Editor
{
    public enum PS2ValidationSeverity
    {
        Info,
        Warning,
        Error
    }

    /// <summary>One validator finding: severity, human-readable message, actionable fix hint.</summary>
    public sealed class PS2ValidationMessage
    {
        public readonly PS2ValidationSeverity severity;
        public readonly string message;
        public readonly string fixHint;

        public PS2ValidationMessage(PS2ValidationSeverity severity, string message, string fixHint)
        {
            this.severity = severity;
            this.message = message;
            this.fixHint = fixHint;
        }
    }

    /// <summary>
    /// Editor-side project validator (plan section 7: fail the build with actionable
    /// errors rather than producing a broken ISO). Run by the Build Profiles window
    /// on every repaint and by the pipeline's Validate stage.
    /// </summary>
    public static class PS2ProjectValidator
    {
        public static List<PS2ValidationMessage> Validate(PS2BuildProfile profile)
        {
            List<PS2ValidationMessage> messages = new List<PS2ValidationMessage>();

            if (profile == null)
            {
                messages.Add(Error(
                    "No PS2 build profile assigned.",
                    "Create one via Assets > Create > Build Profiles > PlayStation 2 and assign it in Window > PS2 > Build Profiles."));
                return messages;
            }

            ValidateScenes(profile, messages);
            ValidateOutputDirectory(profile, messages);
            ValidateToolchain(messages);
            ScanApiUsage(profile, messages);

            return messages;
        }

        public static bool HasErrors(List<PS2ValidationMessage> messages)
        {
            if (messages == null)
            {
                return false;
            }
            for (int i = 0; i < messages.Count; i++)
            {
                if (messages[i].severity == PS2ValidationSeverity.Error)
                {
                    return true;
                }
            }
            return false;
        }

        private static void ValidateScenes(PS2BuildProfile profile, List<PS2ValidationMessage> messages)
        {
            if (profile.scenes == null || profile.scenes.Count == 0)
            {
                messages.Add(Error(
                    "The build profile contains no scenes.",
                    "Add at least one SceneAsset to the profile's scene list; the first scene boots."));
                return;
            }

            for (int i = 0; i < profile.scenes.Count; i++)
            {
                SceneAsset scene = profile.scenes[i];
                if (scene == null)
                {
                    messages.Add(Error(
                        "Scene list entry " + i + " is empty.",
                        "Assign a SceneAsset to the entry or remove it from the list."));
                    continue;
                }
                string path = AssetDatabase.GetAssetPath(scene);
                if (string.IsNullOrEmpty(path) || !File.Exists(path))
                {
                    messages.Add(Error(
                        "Scene '" + scene.name + "' (entry " + i + ") does not exist on disk.",
                        "The asset appears to have been deleted or moved outside Unity. Remove it from the list or restore the file."));
                }
            }
        }

        private static void ValidateOutputDirectory(PS2BuildProfile profile, List<PS2ValidationMessage> messages)
        {
            if (string.IsNullOrEmpty(profile.outputDirectory) || profile.outputDirectory.Trim().Length == 0)
            {
                messages.Add(Error(
                    "Output directory is not set.",
                    "Set Output Directory in the profile settings (relative to the project root, or an absolute path)."));
            }
        }

        private static void ValidateToolchain(List<PS2ValidationMessage> messages)
        {
            PS2ToolResult il2cpp = PS2ToolchainLocator.Il2cppExe;
            if (!il2cpp.ok)
            {
                messages.Add(Error("il2cpp compiler is missing.", il2cpp.error));
            }

            PS2ToolResult bcl = PS2ToolchainLocator.UnityAotBclDir;
            if (!bcl.ok)
            {
                messages.Add(Error("unityaot base class library is missing.", bcl.error));
            }

            PS2ToolResult ps2dev = PS2ToolchainLocator.Ps2DevRoot;
            if (!ps2dev.ok)
            {
                messages.Add(Warning(
                    "ps2dev cross-toolchain not found; the NativeBuild stage will fail.",
                    "Run tools/ps2dev/install.ps1 (the install may still be in progress) or set the PS2DEV environment variable."));
            }
        }

        private static void ScanApiUsage(PS2BuildProfile profile, List<PS2ValidationMessage> messages)
        {
            // Plan 13.5's API-usage scan, unblocked now that the section is
            // available. PS2ApiScanner documents exactly what a source-text
            // scan can and cannot see; the authoritative check remains the C++
            // link, which fails on a missing symbol.
            foreach (PS2ContentValidator.Finding finding in PS2ApiScanner.Scan("."))
            {
                messages.Add(new PS2ValidationMessage(
                    finding.severity, finding.message,
                    "See docs/supported-api.md for the supported subset."));
            }
        }

        /// <summary>
        /// The build-time entry point: profile checks plus the content scan
        /// over the scenes actually being built. Throws PS2BuildException on
        /// any error, so the pipeline fails before doing expensive work.
        /// </summary>
        public static void ValidateForBuild(PS2BuildContext ctx)
        {
            var errors = new List<string>();

            foreach (PS2ContentValidator.Finding finding in PS2ContentValidator.Validate(ctx))
            {
                if (finding.severity == PS2ValidationSeverity.Error)
                {
                    errors.Add(finding.message);
                    UnityEngine.Debug.LogError("[PS2 Build] " + finding.message,
                                               finding.context);
                }
                else
                {
                    ctx.Warn(finding.message);
                }
            }

            foreach (PS2ContentValidator.Finding finding in PS2ApiScanner.Scan(ctx.ProjectRoot))
            {
                if (finding.severity == PS2ValidationSeverity.Error)
                {
                    errors.Add(finding.message);
                    UnityEngine.Debug.LogError("[PS2 Build] " + finding.message);
                }
                else
                {
                    ctx.Warn(finding.message);
                }
            }

            if (errors.Count > 0)
            {
                throw new PS2BuildException(
                    $"Validation found {errors.Count} " +
                    (errors.Count == 1 ? "problem" : "problems") +
                    " that would produce a broken build. Each is logged above " +
                    "with the asset it refers to:\n  - " +
                    string.Join("\n  - ", errors));
            }
        }

        private static PS2ValidationMessage Error(string message, string fixHint)
        {
            return new PS2ValidationMessage(PS2ValidationSeverity.Error, message, fixHint);
        }

        private static PS2ValidationMessage Warning(string message, string fixHint)
        {
            return new PS2ValidationMessage(PS2ValidationSeverity.Warning, message, fixHint);
        }
    }
}
