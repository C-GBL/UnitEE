using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;

namespace Ps2.Editor
{
    /// <summary>
    /// Scans user scripts for API this runtime does not implement, and for the
    /// conformance traps in plan section 7.4 (plan 13.5: "Roslyn syntax walk
    /// over the user's scripts").
    ///
    /// WHAT THIS IS, precisely, so nobody trusts it further than it deserves:
    /// a SOURCE-TEXT scan, not a semantic one. It has no symbol table, so it
    /// cannot tell UnityEngine.Physics.ComputePenetration from your own class
    /// called Physics, and it cannot see through a using-alias or var. It is a
    /// fast pre-flight that catches the common cases with a good message; the
    /// authoritative check is the C++ link, which fails on a missing symbol.
    ///
    /// A real Roslyn semantic walk needs Microsoft.CodeAnalysis, which is not a
    /// dependency this package takes today. Stating the limitation is better
    /// than implying a rigour that is not there -- a validator that quietly
    /// misses things teaches users to ignore it.
    /// </summary>
    public static class PS2ApiScanner
    {
        private sealed class Rule
        {
            public Regex pattern;
            public PS2ValidationSeverity severity;
            public string message;
        }

        private static readonly List<Rule> Rules = BuildRules();

        private static List<Rule> BuildRules()
        {
            var rules = new List<Rule>();

            void Add(string pattern, PS2ValidationSeverity severity, string message)
            {
                rules.Add(new Rule
                {
                    pattern = new Regex(pattern, RegexOptions.Compiled),
                    severity = severity,
                    message = message,
                });
            }

            // Physics: everything ADR-009 says the solver does not have.
            Add(@"\bPhysics\.RaycastAll\b", PS2ValidationSeverity.Error,
                "Physics.RaycastAll allocates an array per call and needs a " +
                "multi-hit query this runtime does not implement. Use " +
                "Physics.Raycast for the nearest hit, or OverlapSphereNonAlloc " +
                "into a buffer you own. (supported-api deviation 19)");
            Add(@"\bPhysics\.(OverlapBox|OverlapCapsule|CapsuleCast|ComputePenetration)\b",
                PS2ValidationSeverity.Error,
                "This query is not implemented. Available: Raycast, SphereCast, " +
                "OverlapSphereNonAlloc. (supported-api deviation 19)");
            Add(@"\bAddComponent<\s*(Box|Sphere|Capsule|Mesh)Collider\s*>",
                PS2ValidationSeverity.Error,
                "Colliders are baked offline into world-space collision, so they " +
                "cannot be created at runtime. Author the collider in the scene. " +
                "(supported-api deviation 18)");
            Add(@"\b(HingeJoint|FixedJoint|SpringJoint|ConfigurableJoint|CharacterJoint)\b",
                PS2ValidationSeverity.Error,
                "Joints need a constraint solver, which this physics model does " +
                "not have (ADR-009).");
            Add(@"\.(AddTorque|AddExplosionForce|AddForceAtPosition)\s*\(",
                PS2ValidationSeverity.Error,
                "Only Rigidbody.AddForce in Force mode is implemented; rotation " +
                "is not solved from contacts. (supported-api deviation 22)");
            Add(@"\brigidbody\.(constraints|interpolation|collisionDetectionMode|centerOfMass|inertiaTensor)\b",
                PS2ValidationSeverity.Error,
                "This Rigidbody property has no equivalent. freezeRotation is the " +
                "only constraint available. (supported-api deviation 22)");

            // Input: what the DualShock 2 cannot answer.
            Add(@"\bInput\.(GetKey|GetKeyDown|GetKeyUp)\b", PS2ValidationSeverity.Error,
                "There is no keyboard on a PS2. Use Input.GetButton with Unity's " +
                "default button names, or PS2Input.GetButton for a specific pad " +
                "button. (supported-api deviation 11)");
            Add(@"\bInput\.(mousePosition|GetMouseButton|GetMouseButtonDown|GetMouseButtonUp|touches|GetTouch|touchCount|acceleration)\b",
                PS2ValidationSeverity.Error,
                "There is no mouse, touchscreen or accelerometer. Map this to the " +
                "pad via Input or PS2Input. (supported-api deviation 11)");

            // Scene management.
            Add(@"\bSceneManager\.(UnloadSceneAsync|UnloadScene)\b",
                PS2ValidationSeverity.Error,
                "A scene's meshes point into the container it was read from " +
                "(zero copy), so one scene cannot be pulled out of a merged " +
                "world. Load with LoadSceneMode.Single to get back to one scene. " +
                "(supported-api deviation 13)");
            Add(@"\bSceneManager\.LoadScene(Async)?\s*\(\s*\d",
                PS2ValidationSeverity.Error,
                "Scenes are addressed by NAME, not build index: a disc has no " +
                "Build Settings scene list to index into. (supported-api " +
                "deviation 13)");

            // Rendering and assets that have no runtime path.
            Add(@"\b(Resources\.Load|Addressables\.)", PS2ValidationSeverity.Warning,
                "Runtime asset loading goes through the streaming queue and the " +
                "baked containers; Resources.Load has no backing store on the " +
                "disc layout. Reference the asset from a scene instead.");
            Add(@"\bnew\s+(Material|Texture2D|Mesh)\s*\(", PS2ValidationSeverity.Warning,
                "Creating this at runtime allocates on a 4 MB managed heap and " +
                "has no baked GS state. Author it as an asset where possible.");
            Add(@"\b(Shader|ComputeShader|CommandBuffer|RenderTexture|Graphics)\.",
                PS2ValidationSeverity.Error,
                "There are no shaders on the PS2: materials are a fixed set of " +
                "kinds mapped to VU1 microprograms (supported-api, Material " +
                "model).");

            // Conformance traps from plan 7.4.
            Add(@"\b(double|decimal)\s+\w+\s*=", PS2ValidationSeverity.Warning,
                "double is software-emulated on the EE and 20-100x slower than " +
                "float; decimal is worse. Use float unless the precision is " +
                "genuinely required. (supported-api deviations 1 and 2)");
            Add(@"\bSystem\.Reflection\b", PS2ValidationSeverity.Warning,
                "Reflection only survives managed stripping if the types are " +
                "preserved; add a link.xml entry and set it on the build " +
                "profile. (supported-api deviation 15)");
            Add(@"\bTask\.Run\b|\bnew\s+Thread\s*\(", PS2ValidationSeverity.Error,
                "There is no thread pool. async/await runs on a cooperative " +
                "scheduler; use a coroutine.");

            return rules;
        }

        /// <summary>
        /// Scans every .cs file under Assets/ that is not part of a package or
        /// the Editor. Returns findings with file and line so the console entry
        /// is clickable.
        /// </summary>
        public static List<PS2ContentValidator.Finding> Scan(string projectRoot)
        {
            var findings = new List<PS2ContentValidator.Finding>();
            string assets = Path.Combine(projectRoot, "Assets");
            if (!Directory.Exists(assets))
                return findings;

            foreach (string file in Directory.GetFiles(assets, "*.cs", SearchOption.AllDirectories))
            {
                string normalised = file.Replace('\\', '/');
                // Editor scripts run in the Editor, not on the console, so the
                // constraints do not apply to them.
                if (normalised.Contains("/Editor/") || normalised.Contains("/Plugins/"))
                    continue;

                string[] lines;
                try
                {
                    lines = File.ReadAllLines(file);
                }
                catch (IOException)
                {
                    continue;
                }

                for (int i = 0; i < lines.Length; i++)
                {
                    string line = lines[i];
                    // Cheap comment filter. Not exact (it misses block comments
                    // and string literals) but it removes the common false
                    // positive of a rule matching its own explanation in a
                    // comment.
                    string trimmed = line.TrimStart();
                    if (trimmed.StartsWith("//") || trimmed.StartsWith("*"))
                        continue;

                    foreach (Rule rule in Rules)
                    {
                        if (!rule.pattern.IsMatch(line))
                            continue;
                        findings.Add(new PS2ContentValidator.Finding
                        {
                            severity = rule.severity,
                            message = $"{normalised}({i + 1}): {rule.message}",
                            context = null,
                        });
                    }
                }
            }
            return findings;
        }
    }
}
