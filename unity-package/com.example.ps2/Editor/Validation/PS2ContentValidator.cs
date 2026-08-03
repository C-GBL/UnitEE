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
        /// <summary>
        /// Components the runtime knows about at all. Anything outside this
        /// set has no equivalent whatsoever and is a hard error.
        /// </summary>
        private static readonly HashSet<string> KnownComponents = new HashSet<string>
        {
            "Transform", "RectTransform", "MeshFilter", "MeshRenderer",
            "SkinnedMeshRenderer", "Camera", "Light", "Animator", "Animation",
            "AudioSource", "AudioListener", "BoxCollider", "SphereCollider",
            "CapsuleCollider", "MeshCollider", "Rigidbody", "CharacterController",
        };

        /// <summary>
        /// Components the SCENE EXPORTER actually writes into the container.
        ///
        /// This set exists because of a bug worth not repeating. Rigidbody was
        /// in the supported list, the shim implemented it, the physics engine
        /// simulated it -- and the scene exporter carried no record of it, so
        /// GetComponent&lt;Rigidbody&gt;() returned null on target and the
        /// user's script died with a NullReferenceException on the frame they
        /// pressed a button. Every piece was present and the chain still had
        /// a hole in it.
        ///
        /// A validator that only asks "is this component supported?" cannot
        /// see that hole, because every individual answer is yes. So the
        /// question asked here is the end-to-end one: does this component
        /// survive the trip into the .p2b?
        /// </summary>
        private static readonly HashSet<string> ExportedComponents = new HashSet<string>
        {
            "Transform", "RectTransform", "MeshFilter", "MeshRenderer",
            "Camera", "Light", "Rigidbody",
            "BoxCollider", "SphereCollider", "CapsuleCollider", "MeshCollider",
            // M12.5: task 1 (both rig kinds) and task 2.
            "SkinnedMeshRenderer", "Animator", "AudioSource", "AudioListener",
            "PS2ParticleSystem",
            // M12.5 task 5: the uGUI subset, plus the harness components a
            // canvas drags in that export as nothing and harm nothing.
            "Canvas", "CanvasRenderer", "Image", "RawImage", "Text",
            "Button", "Slider", "CanvasScaler", "GraphicRaycaster",
        };

        /// <summary>
        /// Known-but-dropped components, each with what actually happens on
        /// target. The text is the whole value of this check: "unsupported"
        /// sends someone looking for a workaround, "the exporter has no path
        /// for it, so GetComponent returns null" tells them what to expect.
        /// </summary>
        private static readonly Dictionary<string, string> DropReasons =
            new Dictionary<string, string>
            {
                ["EventSystem"] =
                    "there is no pointer on a DualShock 2. Focus moves with " +
                    "the D-pad through PS2UINavigation (automatic for " +
                    "exported Buttons and Sliders); the EventSystem object " +
                    "is simply not needed and does nothing on target.",
                ["StandaloneInputModule"] =
                    "rides EventSystem; PS2UINavigation replaces both.",
                ["InputSystemUIInputModule"] =
                    "rides EventSystem; PS2UINavigation replaces both.",
                ["ParticleSystem"] =
                    "Unity's ParticleSystem (curves, sub-emitters, GPU sim) " +
                    "has no PS2 equivalent. Use the PS2ParticleSystem " +
                    "component (Add Component > PS2), the constrained " +
                    "replacement ADR-011 defines: rate/burst emission, " +
                    "sphere/cone/box shapes, linear size and colour ramps.",
                ["ParticleSystemRenderer"] =
                    "rides Unity's ParticleSystem; PS2ParticleSystem draws " +
                    "itself.",
                ["Animation"] =
                    "the legacy Animation component is not implemented; the " +
                    "runtime animates through Animator and baked clips only.",
                ["AudioSource"] =
                    "audio is exported as a separate SND section with no " +
                    "per-entity binding, so the source will not play by itself. " +
                    "Drive it from a script through the PS2Audio API.",
                ["AudioListener"] =
                    "there is a single implicit listener at the active camera; " +
                    "the component itself does nothing.",
                ["CharacterController"] =
                    "the exporter carries no record of its radius/height/slope " +
                    "settings, so GetComponent<CharacterController>() returns " +
                    "null. Add one from a script with " +
                    "gameObject.AddComponent<CharacterController>() and set its " +
                    "properties there.",
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

                // Exactly one AudioListener, as Unity itself warns: the mixer
                // has one listener pose, and the loader keeps the FIRST it
                // meets, so a second is authored confusion, not variety.
                var listeners = UnityEngine.Object.FindObjectsByType<AudioListener>(
                    FindObjectsSortMode.None);
                if (listeners.Length > 1)
                {
                    findings.Add(Error(
                        $"{scenePath}: {listeners.Length} AudioListeners " +
                        $"('{PathOf(listeners[0].gameObject)}' and " +
                        $"'{PathOf(listeners[1].gameObject)}'). A scene has ONE; " +
                        "the loader keeps the first and the others do nothing.",
                        listeners[1]));
                }
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
                if (!KnownComponents.Contains(type))
                {
                    findings.Add(Error(
                        $"{scenePath}: '{PathOf(go)}' has a {type}, which this runtime " +
                        "does not implement. See docs/supported-api.md for the " +
                        "supported component list; remove it or replace it with a " +
                        "supported equivalent.", go));
                }
                else if (!ExportedComponents.Contains(type))
                {
                    string why = DropReasons.TryGetValue(type, out string reason)
                        ? reason
                        : "the scene exporter has no path for it.";
                    findings.Add(Error(
                        $"{scenePath}: '{PathOf(go)}' has a {type} that WILL NOT be " +
                        $"exported: {why} The build will otherwise succeed, so this " +
                        "would only show up as wrong behaviour on the console. See " +
                        "docs/supported-api.md.", go));
                }
            }

            // A directional Light is the only kind the container carries: the
            // VU1 lighting program takes a direction and a colour, and there
            // is no per-pixel path a point light could use.
            var light = go.GetComponent<Light>();
            if (light != null && light.type != LightType.Directional)
            {
                findings.Add(Error(
                    $"{scenePath}: '{PathOf(go)}' has a {light.type} Light. Only " +
                    "Directional lights are exported -- vertex lighting on VU1 " +
                    "takes a direction, so a point or spot light has no " +
                    "equivalent. The object will simply be unlit.", light));
            }

            // The failure this whole check exists for, in its remaining form:
            // a Rigidbody that IS exported but can never move, because this
            // solver drives bodies through their collider.
            var body = go.GetComponent<Rigidbody>();
            if (body != null && go.GetComponent<Collider>() == null)
            {
                findings.Add(Error(
                    $"{scenePath}: '{PathOf(go)}' has a Rigidbody but no Collider. " +
                    "This solver moves a body through its collider (ADR-009), so " +
                    "the body would integrate a velocity and never move. Add a " +
                    "Box, Sphere or CapsuleCollider. Deviation 23 in " +
                    "docs/supported-api.md.", body));
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
