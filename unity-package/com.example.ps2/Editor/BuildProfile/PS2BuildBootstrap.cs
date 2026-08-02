using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace Ps2.Editor
{
    /// <summary>
    /// Creates a default build profile for a project that has none (plan 13.2's
    /// "Platform Browser-style empty state").
    ///
    /// Also the batch-mode entry point CI uses to bootstrap a project before
    /// building it, so the D1 acceptance ("install the package, press Build and
    /// Run") can be exercised headlessly rather than only by hand.
    /// </summary>
    public static class PS2BuildBootstrap
    {
        public const string DefaultProfilePath = "Assets/Settings/PS2/Default.asset";

        [MenuItem("Window/PS2/Create Default Build Profile")]
        public static void CreateDefaultProfile()
        {
            PS2BuildProfile profile = CreateOrLoad(DefaultProfilePath);
            Selection.activeObject = profile;
            EditorGUIUtility.PingObject(profile);
        }

        /// <summary>
        /// Loads the profile at 'path', creating it from the project's scenes
        /// if it is not there. Safe to call repeatedly: an existing profile is
        /// returned untouched, so a CI run never silently rewrites settings
        /// somebody chose.
        /// </summary>
        public static PS2BuildProfile CreateOrLoad(string path)
        {
            var existing = AssetDatabase.LoadAssetAtPath<PS2BuildProfile>(path);
            if (existing != null)
                return existing;

            string directory = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(directory) && !Directory.Exists(directory))
                Directory.CreateDirectory(directory);

            var profile = ScriptableObject.CreateInstance<PS2BuildProfile>();

            // Seed the scene list from the Build Settings scenes when there are
            // any, so a project that already knows its scene order keeps it.
            // Otherwise take the open scene, which is what a first-time user
            // has in front of them.
            var scenes = EditorBuildSettings.scenes
                .Where(s => s.enabled)
                .Select(s => AssetDatabase.LoadAssetAtPath<SceneAsset>(s.path))
                .Where(s => s != null)
                .ToList();
            if (scenes.Count == 0)
            {
                string active = EditorSceneManager.GetActiveScene().path;
                if (!string.IsNullOrEmpty(active))
                {
                    var asset = AssetDatabase.LoadAssetAtPath<SceneAsset>(active);
                    if (asset != null)
                        scenes.Add(asset);
                }
            }
            if (scenes.Count == 0)
            {
                // Last resort: every scene in the project, alphabetically. In
                // batch mode there is no open scene and a fresh project has
                // nothing in Build Settings, so without this the bootstrap
                // produces a profile with no scenes and the build fails on
                // its own output.
                foreach (string guid in AssetDatabase.FindAssets("t:SceneAsset"))
                {
                    string scenePath = AssetDatabase.GUIDToAssetPath(guid);
                    if (!scenePath.StartsWith("Assets/"))
                        continue; // package scenes are not the user's game
                    var asset = AssetDatabase.LoadAssetAtPath<SceneAsset>(scenePath);
                    if (asset != null)
                        scenes.Add(asset);
                }
                scenes = scenes.OrderBy(s => s.name, System.StringComparer.Ordinal).ToList();
            }
            profile.scenes = scenes;
            profile.productName = Application.productName;

            AssetDatabase.CreateAsset(profile, path);
            AssetDatabase.SaveAssets();
            Debug.Log($"[PS2 Build] created a default build profile at {path} with " +
                      $"{scenes.Count} scene(s).");
            return profile;
        }

        /// <summary>
        /// Batch-mode: create the default profile if needed, then build it.
        /// Used by CI and by the M12 acceptance run.
        ///
        ///   Unity -batchmode -quit -projectPath &lt;proj&gt; \
        ///         -executeMethod Ps2.Editor.PS2BuildBootstrap.BootstrapAndBuild
        /// </summary>
        public static void BootstrapAndBuild()
        {
            int exitCode = 1;
            try
            {
                PS2BuildProfile profile = CreateOrLoad(DefaultProfilePath);
                PS2BuildReport report = PS2BuildPipeline.Build(profile);
                exitCode = report.succeeded ? 0 : 1;
            }
            catch (System.Exception e)
            {
                Debug.LogError("[PS2 Build] " + e);
            }
            if (Application.isBatchMode)
                EditorApplication.Exit(exitCode);
        }
    }
}
