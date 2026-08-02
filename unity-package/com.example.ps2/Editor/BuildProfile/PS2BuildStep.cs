using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;

namespace Ps2.Editor
{
    /// <summary>
    /// One build step (plan section 13.3). The interface exists so the pipeline
    /// can report, cache and re-run each step individually rather than treating
    /// the build as one opaque operation -- which is the difference between "the
    /// build failed" and "step 6 of 10 failed, here is why, and here is the
    /// command to reproduce it".
    /// </summary>
    public interface IPS2BuildStep
    {
        string Name { get; }

        /// <summary>
        /// True when this step's outputs are newer than its inputs and can be
        /// skipped. Returning false is always safe; returning true wrongly is
        /// how an incremental build ships stale content, so every
        /// implementation errs towards rebuilding.
        /// </summary>
        bool IsUpToDate(PS2BuildContext ctx);

        void Run(PS2BuildContext ctx);
    }

    /// <summary>What a step did, for the report and the console.</summary>
    public enum PS2StepOutcome
    {
        Ran,
        Skipped,
        Failed,
    }

    public sealed class PS2StepResult
    {
        public string name;
        public PS2StepOutcome outcome;
        public long milliseconds;
        public string message = "";
    }

    /// <summary>
    /// Everything a step needs and everything it produces. Passing one context
    /// rather than threading arguments means a new step can be inserted without
    /// touching the signatures of the ones around it.
    /// </summary>
    public sealed class PS2BuildContext
    {
        public PS2BuildProfile Profile;

        /// <summary>Project root (the directory containing Assets/).</summary>
        public string ProjectRoot;

        /// <summary>Where the finished game.elf / game.iso go.</summary>
        public string OutputDirectory;

        /// <summary>Scratch: Temp/PS2Build/. Safe to delete; Clean does.</summary>
        public string IntermediateDirectory;

        /// <summary>Repository root, where runtime/ and tools/ live.</summary>
        public string PackageRoot;

        public PS2ToolchainInfo Toolchain;

        /// <summary>Set by ResolveContent; consumed by the export steps.</summary>
        public readonly List<string> ScenePaths = new List<string>();

        /// <summary>Files the Package step will put on the disc.</summary>
        public readonly List<string> ContentFiles = new List<string>();

        public readonly List<PS2StepResult> Results = new List<PS2StepResult>();
        public readonly List<string> Warnings = new List<string>();

        /// <summary>Content hashes from the previous build, for IsUpToDate.</summary>
        public PS2BuildCache Cache;

        public bool ForceRebuild;

        public string ContentDirectory => Path.Combine(IntermediateDirectory, "content");
        public string Il2cppOutputDirectory => Path.Combine(IntermediateDirectory, "il2cppOutput");
        public string NativeBuildDirectory => Path.Combine(IntermediateDirectory, "native");

        /// <summary>Set by the native step when a game ELF was actually linked.</summary>
        public string GameBuildDirectory;

        public string ElfPath => Path.Combine(OutputDirectory, "game.elf");
        public string IsoPath => Path.Combine(OutputDirectory, "game.iso");

        public void Warn(string message)
        {
            Warnings.Add(message);
            UnityEngine.Debug.LogWarning("[PS2 Build] " + message);
        }
    }

    /// <summary>
    /// Content hashes keyed by step name, persisted next to the intermediates
    /// (plan 13.3 task 6: "content-hash every input ... skip export for
    /// unchanged assets").
    ///
    /// Hashes rather than timestamps on purpose. A timestamp says a file was
    /// touched; a hash says its content changed. Reverting a file with git
    /// updates its timestamp but restores its content, and a timestamp-based
    /// cache would rebuild the world for nothing -- or worse, a checkout that
    /// preserves timestamps would skip a rebuild that was needed.
    /// </summary>
    [Serializable]
    public sealed class PS2BuildCache
    {
        [Serializable]
        public sealed class Entry
        {
            public string step;
            public string hash;
        }

        public List<Entry> entries = new List<Entry>();

        public string Get(string step)
        {
            foreach (Entry e in entries)
            {
                if (e.step == step)
                    return e.hash;
            }
            return null;
        }

        public void Set(string step, string hash)
        {
            foreach (Entry e in entries)
            {
                if (e.step == step)
                {
                    e.hash = hash;
                    return;
                }
            }
            entries.Add(new Entry { step = step, hash = hash });
        }

        public static PS2BuildCache Load(string path)
        {
            try
            {
                if (File.Exists(path))
                    return UnityEngine.JsonUtility.FromJson<PS2BuildCache>(
                        File.ReadAllText(path));
            }
            catch (Exception e)
            {
                // A corrupt cache must never fail a build -- it just means
                // everything rebuilds, which is the safe direction.
                UnityEngine.Debug.LogWarning(
                    "[PS2 Build] build cache unreadable, rebuilding everything: " + e.Message);
            }
            return new PS2BuildCache();
        }

        public void Save(string path)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            File.WriteAllText(path, UnityEngine.JsonUtility.ToJson(this, true));
        }
    }

    /// <summary>Hashing helpers shared by the steps.</summary>
    public static class PS2ContentHash
    {
        /// <summary>
        /// Hash of a set of files: path, size and content. Missing files
        /// contribute a marker rather than throwing, so a deleted input
        /// correctly invalidates the cache instead of crashing the check.
        /// </summary>
        public static string OfFiles(IEnumerable<string> paths)
        {
            using (var sha = System.Security.Cryptography.SHA256.Create())
            {
                var buffer = new MemoryStream();
                foreach (string path in paths)
                {
                    byte[] name = Encoding.UTF8.GetBytes(path.Replace('\\', '/'));
                    buffer.Write(name, 0, name.Length);
                    if (!File.Exists(path))
                    {
                        byte[] missing = Encoding.UTF8.GetBytes("<missing>");
                        buffer.Write(missing, 0, missing.Length);
                        continue;
                    }
                    byte[] content = File.ReadAllBytes(path);
                    buffer.Write(content, 0, content.Length);
                }
                buffer.Position = 0;
                return ToHex(sha.ComputeHash(buffer));
            }
        }

        public static string OfString(string text)
        {
            using (var sha = System.Security.Cryptography.SHA256.Create())
                return ToHex(sha.ComputeHash(Encoding.UTF8.GetBytes(text ?? "")));
        }

        /// <summary>
        /// The profile's own settings, so changing the video mode or the heap
        /// size invalidates the steps that depend on them. JSON of the asset is
        /// enough: it captures every serialised field without this needing to
        /// know which ones exist.
        /// </summary>
        public static string OfProfile(PS2BuildProfile profile) =>
            OfString(UnityEngine.JsonUtility.ToJson(profile));

        private static string ToHex(byte[] bytes)
        {
            var sb = new StringBuilder(bytes.Length * 2);
            foreach (byte b in bytes)
                sb.Append(b.ToString("x2"));
            return sb.ToString();
        }
    }

    /// <summary>Running an external tool and streaming its output to the console.</summary>
    public static class PS2Process
    {
        /// <summary>
        /// Runs 'exe' with 'args' in 'workingDirectory'. Returns the exit code
        /// and captures stdout+stderr. Output is streamed rather than read at
        /// the end so a long native build shows progress instead of appearing
        /// to hang (plan 13.3 step 8: "stream compiler output into the Unity
        /// console").
        /// </summary>
        public static int Run(string exe, string args, string workingDirectory,
                              out string output, Action<string> onLine = null,
                              int timeoutMs = 0)
        {
            var sb = new StringBuilder();
            var psi = new ProcessStartInfo(exe, args)
            {
                WorkingDirectory = workingDirectory ?? Environment.CurrentDirectory,
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
            };
            using (var process = new Process { StartInfo = psi })
            {
                DataReceivedEventHandler handler = (s, e) =>
                {
                    if (e.Data == null)
                        return;
                    lock (sb)
                        sb.AppendLine(e.Data);
                    onLine?.Invoke(e.Data);
                };
                process.OutputDataReceived += handler;
                process.ErrorDataReceived += handler;
                process.Start();
                process.BeginOutputReadLine();
                process.BeginErrorReadLine();
                if (timeoutMs > 0)
                {
                    if (!process.WaitForExit(timeoutMs))
                    {
                        try { process.Kill(); } catch { }
                        lock (sb)
                            output = sb.ToString();
                        return -1;
                    }
                }
                else
                {
                    process.WaitForExit();
                }
                lock (sb)
                    output = sb.ToString();
                return process.ExitCode;
            }
        }
    }
}
