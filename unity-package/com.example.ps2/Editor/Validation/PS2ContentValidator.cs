using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace Ps2.Editor
{
    /// <summary>
    /// The project-content half of plan section 13.5: scan the scenes actually
    /// being built for things this runtime cannot do, and say so BEFORE forty
    /// minutes of IL2CPP.
    ///
    /// Two rules govern the messages, both from the plan:
    ///   - "Every message must name the asset, explain the constraint, and link
    ///     to the doc section."
    ///   - "Write the message text as carefully as the code."
    ///
    /// So every finding here carries the object's full scene path, what the
    /// limit is and why it exists, and which supported-api deviation or plan
    /// section covers it. A validator that says "unsupported component" and
    /// leaves the user to find it is not doing its job.
    /// </summary>
    public static class PS2ContentValidator
    {
        /// <summary>Components with a real equivalent in the shim.</summary>
        private static readonly HashSet<string> SupportedComponents = new HashSet<string>
        {
            "Transform", "RectTransform", "MeshFilter", "MeshRenderer",
            "SkinnedMeshRenderer", "Camera", "Light", "Animator", "Animation",
            "AudioSource", "AudioListener", "BoxCollider", "SphereCollider",
            "CapsuleCollider", "MeshCollider", "Rigidbody", "CharacterController",
        };

        public sealed class Finding
        {
            public PS2ValidationSeverity severity;
            public string message;
            public UnityEngine.Object context; // makes the console entry clickable
        }

        public static List<Finding> Validate(PS2BuildContext ctx)
        {
            var findings = new List<Finding>();
            string active = EditorSceneManager.GetActiveScene().path;

            foreach (string scenePath in ctx.ScenePaths)
            {
                EditorSceneManager.OpenScene(scenePath, OpenSceneMode.Single);
                var scene = EditorSceneManager.GetActiveScene();
                foreach (GameObject root in scene.GetRootGameObjects())
                    WalkObject(root, scenePath, ctx, findings);
                ValidateSceneBudget(scene, scenePath, ctx, findings);
            }

            if (!string.IsNullOrEmpty(active) && File.Exists(active))
                EditorSceneManager.OpenScene(active, OpenSceneMode.Single);

            ValidateTextures(ctx, findings);
            return findings;
        }

        private static void WalkObject(GameObject go, string scenePath,
                                       PS2BuildContext ctx, List<Finding> findings)
        {
            foreach (Component c in go.GetComponents<Component>())
            {
                if (c == null)
                {
                    findings.Add(Error(
                        $"{scenePath}: '{PathOf(go)}' has a MISSING script component. " +
                        "A missing script cannot be exported and usually means a " +
                        "deleted or renamed class; remove the component or restore " +
                        "the script.", go));
                    continue;
                }
                if (c is MonoBehaviour)
                    continue; // user scripts are the point; the shim validates their API use

                string type = c.GetType().Name;
                if (!SupportedComponents.Contains(type))
                {
                    findings.Add(Error(
                        $"{scenePath}: '{PathOf(go)}' has a {type}, which this runtime " +
                        "does not implement. See docs/supported-api.md for the " +
                        "supported component list; remove it or replace it with a " +
                        "supported equivalent.", go));
                }
            }

            var filter = go.GetComponent<MeshFilter>();
            if (filter != null && filter.sharedMesh != null && !filter.sharedMesh.isReadable)
            {
                findings.Add(Error(
                    $"{scenePath}: mesh '{filter.sharedMesh.name}' on '{PathOf(go)}' is " +
                    "not readable, so the exporter cannot get its vertices. Enable " +
                    "Read/Write Enabled in the model's import settings.", filter.sharedMesh));
            }

            var mc = go.GetComponent<MeshCollider>();
            if (mc != null && mc.convex)
            {
                findings.Add(Error(
                    $"{scenePath}: '{PathOf(go)}' has a convex MeshCollider. Baked " +
                    "collision is static world-space geometry (ADR-009), so a convex " +
                    "hull for a moving body has no equivalent -- use a " +
                    "Box/Sphere/CapsuleCollider on anything with a Rigidbody. " +
                    "Deviation 18 in docs/supported-api.md.", mc));
            }

            var renderer = go.GetComponent<Renderer>();
            if (renderer != null)
            {
                foreach (Material m in renderer.sharedMaterials)
                {
                    if (m == null)
                    {
                        findings.Add(Warning(
                            $"{scenePath}: '{PathOf(go)}' has an empty material slot; " +
                            "it will export with the default unlit material.", go));
                        continue;
                    }
                    if (m.shader == null)
                    {
                        findings.Add(Error(
                            $"{scenePath}: material '{m.name}' on '{PathOf(go)}' has no " +
                            "shader.", m));
                    }
                }
            }

            foreach (Transform child in go.transform)
                WalkObject(child.gameObject, scenePath, ctx, findings);
        }

        private static void ValidateSceneBudget(UnityEngine.SceneManagement.Scene scene,
                                                string scenePath, PS2BuildContext ctx,
                                                List<Finding> findings)
        {
            // Must match kMaxEntities in runtime/include/ps2ur/p2b_scene.h. A
            // scene that overflows fails at LOAD on target, which is a much
            // worse place to find out than here.
            const int kMaxEntities = 640;
            int count = 0;
            foreach (GameObject root in scene.GetRootGameObjects())
                count += root.GetComponentsInChildren<Transform>(true).Length;

            if (count > kMaxEntities)
            {
                findings.Add(Error(
                    $"{scenePath} has {count} GameObjects; the runtime's entity table " +
                    $"holds {kMaxEntities} (kMaxEntities in p2b_scene.h). The scene " +
                    "would fail to load on target. Split it, or load part of it " +
                    "additively at runtime.", null));
            }
            else if (count > kMaxEntities * 8 / 10)
            {
                findings.Add(Warning(
                    $"{scenePath} has {count} GameObjects, over 80% of the " +
                    $"{kMaxEntities} the entity table holds.", null));
            }
        }

        private static void ValidateTextures(PS2BuildContext ctx, List<Finding> findings)
        {
            int max = ctx.Profile.textureMaxSize;
            var seen = new HashSet<string>();

            foreach (string scenePath in ctx.ScenePaths)
            {
                foreach (string dep in AssetDatabase.GetDependencies(scenePath, true))
                {
                    if (!seen.Add(dep))
                        continue;
                    var texture = AssetDatabase.LoadAssetAtPath<Texture2D>(dep);
                    if (texture == null)
                        continue;

                    bool potW = (texture.width & (texture.width - 1)) == 0;
                    bool potH = (texture.height & (texture.height - 1)) == 0;
                    if (!potW || !potH)
                    {
                        string message =
                            $"Texture '{dep}' is {texture.width}x{texture.height}, which " +
                            "is not a power of two. The GS addresses textures by log2 " +
                            "dimensions, so a non-power-of-two size cannot be expressed " +
                            "in TEX0 at all.";
                        findings.Add(ctx.Profile.strictContent
                                         ? Error(message, texture)
                                         : Warning(message + " It will be resized on export.",
                                                   texture));
                    }
                    else if (texture.width > max || texture.height > max)
                    {
                        string message =
                            $"Texture '{dep}' is {texture.width}x{texture.height}, over " +
                            $"the profile's {max} limit.";
                        findings.Add(ctx.Profile.strictContent
                                         ? Error(message, texture)
                                         : Warning(message + " It will be downscaled on export.",
                                                   texture));
                    }
                }
            }
        }

        internal static string PathOf(GameObject go)
        {
            string path = go.name;
            Transform t = go.transform.parent;
            while (t != null)
            {
                path = t.name + "/" + path;
                t = t.parent;
            }
            return path;
        }

        private static Finding Error(string message, UnityEngine.Object context) =>
            new Finding
            {
                severity = PS2ValidationSeverity.Error,
                message = message,
                context = context,
            };

        private static Finding Warning(string message, UnityEngine.Object context) =>
            new Finding
            {
                severity = PS2ValidationSeverity.Warning,
                message = message,
                context = context,
            };
    }
}
