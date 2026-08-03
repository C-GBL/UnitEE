// Real Unity rigs -> the M9 animation sections (M12.5 task 1).
//
// P2bAnimExporter has done the hard part since M9: quantised clips sampled
// through Unity's own evaluator, topologically ordered skeletons, palette
// batching for vu_skin. All of it took assets as arguments, and nothing ever
// handed it any. The only caller was PS2SkinExportMenu, which builds a
// procedural 24-bone tube in code for the M9 acceptance scene -- so a user
// with an imported character, an AnimatorController and a SkinnedMeshRenderer
// had no path at all. The runtime was finished and unreachable (ADR-010).
//
// This is that path: walk the scene, find the rigs, read the controller
// assets, and produce exactly the payload P2bSceneExporter already knows how
// to write.
//
// WHAT IS DELIBERATELY NOT TRANSLATED. An AnimatorController can express
// things this runtime has no equivalent for -- sub-state machines, blend
// trees, layers past the first, transitions with several conditions ANDed
// together. Each is reported by name with what will happen instead. Silently
// flattening a blend tree to its first clip would produce a character that
// animates *nearly* right, which is far worse to debug than one that does not
// animate at all.
using System.Collections.Generic;
using UnityEditor;
using UnityEditor.Animations;
using UnityEngine;

namespace Ps2.Editor
{
    internal static class P2bRigExporter
    {
        // Must match ps2ur::anim:: the same-named constants.
        private const int MaxBones = 192;
        private const int MaxClips = 32;
        private const int MaxStates = 32;
        private const int MaxTransitions = 64;
        private const int MaxParams = 8;

        // Sampling. 30 Hz matches the frame rate the runtime actually runs at;
        // sampling finer would quantise away again on playback.
        private const float SampleRate = 30.0f;
        private const float PositionTolerance = 0.0005f;
        private const float RotationDotTolerance = 0.99995f;
        private const float ScaleTolerance = 0.001f;

        public static P2bSceneExporter.SkinPayload Bake(
            IEnumerable<GameObject> roots, List<string> warnings)
        {
            var all = new List<SkinnedMeshRenderer>();
            foreach (GameObject root in roots)
            {
                if (root == null)
                    continue;
                foreach (SkinnedMeshRenderer smr in
                         root.GetComponentsInChildren<SkinnedMeshRenderer>(true))
                {
                    all.Add(smr);
                }
            }
            if (all.Count == 0)
                return null;

            // Decide which renderers are usable BEFORE committing to any of
            // them. This used to keep only the first one found and abandon the
            // whole scene if that one was unusable, which meant an imported
            // character could be dropped entirely because of the first thing
            // the traversal happened to reach -- on Unity-chan, a bone-less
            // eyebrow plane (verify-log M12.5).
            var usable = new List<SkinnedMeshRenderer>();
            foreach (SkinnedMeshRenderer smr in all)
            {
                if (smr.sharedMesh == null)
                {
                    warnings.Add(
                        $"{PathOf(smr.gameObject)}: SkinnedMeshRenderer with no mesh, " +
                        "skipped.");
                    continue;
                }
                if (smr.bones == null || smr.bones.Length == 0)
                {
                    // Not a broken rig: imported characters use bone-less
                    // SkinnedMeshRenderers as static props parented into the
                    // skeleton -- eyes, brows, mouth planes. They cannot be
                    // skinned, and they must not stop the rest of the
                    // character from exporting.
                    warnings.Add(
                        $"{PathOf(smr.gameObject)}: SkinnedMeshRenderer with no bones. " +
                        "It is parented to the rig but not skinned by it, so it is not " +
                        "exported; give it a MeshFilter + MeshRenderer to draw it.");
                    continue;
                }
                if (!smr.sharedMesh.isReadable)
                {
                    warnings.Add(
                        $"{PathOf(smr.gameObject)}: mesh '{smr.sharedMesh.name}' is not " +
                        "readable, so its bone weights cannot be exported. Enable " +
                        "Read/Write in the model importer.");
                    continue;
                }
                bool nullBone = false;
                foreach (Transform bone in smr.bones)
                {
                    if (bone == null) { nullBone = true; break; }
                }
                if (nullBone)
                {
                    warnings.Add(
                        $"{PathOf(smr.gameObject)}: the bones array has a missing entry, " +
                        "so its vertex weights cannot be resolved. Re-import the model.");
                    continue;
                }
                usable.Add(smr);
            }

            if (usable.Count == 0)
            {
                warnings.Add(
                    $"None of the {all.Count} SkinnedMeshRenderers in the scene could " +
                    "be exported, so no character will be drawn. The reasons are " +
                    "above, one per renderer.");
                return null;
            }

            // ONE skeleton, shared. Every usable renderer contributes its bones
            // to a union: this is how a character is actually authored, with
            // each material a separate renderer over the same rig.
            var unionBones = new List<Transform>();
            var unionBind = new List<Matrix4x4>();
            var boneAt = new Dictionary<Transform, int>();
            foreach (SkinnedMeshRenderer smr in usable)
            {
                Transform[] bones = smr.bones;
                Matrix4x4[] binds = smr.sharedMesh.bindposes;
                for (int i = 0; i < bones.Length; i++)
                {
                    Matrix4x4 bind = i < binds.Length ? binds[i] : Matrix4x4.identity;
                    int at;
                    if (boneAt.TryGetValue(bones[i], out at))
                    {
                        // The bind pose maps MESH space to bone space, so two
                        // renderers agree only if they share a mesh space. An
                        // import does; a hand-assembled rig might not, and the
                        // result would be one piece skinned off its axis --
                        // visible, but easy to blame on the animation.
                        if (!BindposesAgree(unionBind[at], bind))
                        {
                            warnings.Add(
                                $"{PathOf(smr.gameObject)}: bone '{bones[i].name}' has a " +
                                "different bind pose here than on an earlier renderer of " +
                                "the same rig. One shared skeleton is exported, so this " +
                                "mesh may be skinned off its axis. Re-import both meshes " +
                                "from the same model.");
                        }
                        continue;
                    }
                    boneAt[bones[i]] = unionBones.Count;
                    unionBones.Add(bones[i]);
                    unionBind.Add(bind);
                }
            }

            if (unionBones.Count > MaxBones)
            {
                warnings.Add(
                    $"The rig needs {unionBones.Count} bones across " +
                    $"{usable.Count} renderers, over the runtime's limit of " +
                    $"{MaxBones} (anim::kMaxBones). The character will not be " +
                    "exported; reduce the rig, or raise the limit and rebuild " +
                    "the runtime.");
                return null;
            }

            Animator animator = usable[0].GetComponentInParent<Animator>();

            var payload = new P2bSceneExporter.SkinPayload();

            P2bAnimExporter.SkeletonExport skeleton = P2bAnimExporter.ExportSkeleton(
                unionBones.ToArray(), unionBind.ToArray());
            payload.Skeleton = skeleton.Bytes;

            // The clips, and the controller that names them. A rig with an
            // Animator but no controller still exports its skeleton and mesh:
            // it can be posed from script, which is a legitimate thing to do.
            var controller = animator != null
                ? animator.runtimeAnimatorController as AnimatorController
                : null;
            if (animator != null && controller == null &&
                animator.runtimeAnimatorController != null)
            {
                warnings.Add(
                    $"{PathOf(animator.gameObject)}: the Animator's controller is a " +
                    $"{animator.runtimeAnimatorController.GetType().Name}, not an " +
                    "AnimatorController asset. Override controllers are not baked; " +
                    "assign the controller itself.");
            }

            GameObject clipRoot = animator != null ? animator.gameObject
                                                   : usable[0].gameObject;
            var states = new List<P2bAnimExporter.StateExport>();
            var transitions = new List<P2bAnimExporter.TransitionExport>();
            var parameters = new List<string>();
            var stateIndex = new Dictionary<AnimatorState, int>();
            var clipLengths = new List<float>();

            if (controller != null)
            {
                ReadController(controller, clipRoot, skeleton, payload, states,
                               transitions, parameters, stateIndex, clipLengths,
                               warnings);
            }

            // A rig with no usable controller still needs one state, because
            // the runtime's animator always has a current state. An empty
            // controller with no clips would index clip 0 of nothing.
            if (states.Count == 0)
            {
                if (payload.Clips.Count == 0)
                {
                    payload.Clips.Add(P2bAnimExporter.ExportClip(
                        StaticPoseClip(), clipRoot, skeleton.Ordered,
                        skeleton.Index, SampleRate, false, PositionTolerance,
                        RotationDotTolerance, ScaleTolerance));
                    clipLengths.Add(0.0f);
                    if (controller != null)
                    {
                        warnings.Add(
                            $"AnimatorController '{controller.name}' produced no " +
                            "usable states; the character will hold its bind pose.");
                    }
                }
                states.Add(new P2bAnimExporter.StateExport
                {
                    Name = "Default",
                    Clip = 0,
                    Speed = 1.0f,
                    Loop = true,
                });
            }

            payload.Controller = P2bAnimExporter.ExportController(
                states.ToArray(), transitions.ToArray(), parameters.ToArray());

            // One SKMS per distinct mesh; renderers sharing a mesh share the
            // entry, so a crowd of the same character costs one mesh.
            var meshAt = new Dictionary<Mesh, int>();
            var groupAt = new Dictionary<Transform, int>();
            foreach (SkinnedMeshRenderer smr in usable)
            {
                int at;
                if (!meshAt.TryGetValue(smr.sharedMesh, out at))
                {
                    at = payload.SkinnedMeshes.Count;
                    payload.SkinnedMeshes.Add(P2bAnimExporter.ExportSkinnedMesh(
                        smr.sharedMesh, 0, FallbackColour(smr),
                        skeleton.Ordered, skeleton.Index, smr.bones, 0));
                    meshAt[smr.sharedMesh] = at;
                }
                payload.RendererMesh[smr] = at;

                // Group by the Animator that drives this renderer -- the same
                // question Unity answers with GetComponentInParent. Without an
                // Animator anywhere above it, the transform root stands in, so
                // separately rooted characters still animate independently.
                Animator owner = smr.GetComponentInParent<Animator>();
                Transform key = owner != null ? owner.transform
                                              : smr.transform.root;
                int group;
                if (!groupAt.TryGetValue(key, out group))
                {
                    group = groupAt.Count;
                    groupAt[key] = group;
                }
                payload.RendererGroup[smr] = group;
            }
            return payload;
        }

        // Bind poses are inverse bind MATRICES; comparing them elementwise
        // with a loose epsilon is enough to tell "the same import" from "two
        // different mesh spaces", which is the only distinction that matters.
        private static bool BindposesAgree(Matrix4x4 a, Matrix4x4 b)
        {
            for (int i = 0; i < 16; i++)
            {
                if (Mathf.Abs(a[i] - b[i]) > 1e-4f)
                    return false;
            }
            return true;
        }

        // ---- controller -----------------------------------------------------

        private static void ReadController(
            AnimatorController controller, GameObject clipRoot,
            P2bAnimExporter.SkeletonExport skeleton,
            P2bSceneExporter.SkinPayload payload,
            List<P2bAnimExporter.StateExport> states,
            List<P2bAnimExporter.TransitionExport> transitions,
            List<string> parameters, Dictionary<AnimatorState, int> stateIndex,
            List<float> clipLengths, List<string> warnings)
        {
            foreach (AnimatorControllerParameter p in controller.parameters)
            {
                if (parameters.Count >= MaxParams)
                {
                    warnings.Add(
                        $"AnimatorController '{controller.name}' has more than " +
                        $"{MaxParams} parameters (anim::kMaxParams); '{p.name}' and " +
                        "any after it are dropped, and conditions using them will " +
                        "never fire.");
                    break;
                }
                if (p.type == AnimatorControllerParameterType.Int)
                {
                    warnings.Add(
                        $"AnimatorController '{controller.name}': parameter " +
                        $"'{p.name}' is an Int. The runtime stores parameters as " +
                        "floats, so integer comparisons become float ones. Equality " +
                        "on an Int will not behave as it does in the Editor.");
                }
                parameters.Add(p.name);
            }

            if (controller.layers.Length > 1)
            {
                warnings.Add(
                    $"AnimatorController '{controller.name}' has " +
                    $"{controller.layers.Length} layers. Only the first is baked; " +
                    "the runtime blends layers itself but the exporter carries one " +
                    "state machine.");
            }
            if (controller.layers.Length == 0)
                return;

            AnimatorStateMachine machine = controller.layers[0].stateMachine;
            if (machine.stateMachines != null && machine.stateMachines.Length > 0)
            {
                warnings.Add(
                    $"AnimatorController '{controller.name}' contains sub-state " +
                    "machines, which are not baked. Only states in the top-level " +
                    "machine will exist on target.");
            }

            // A clip is exported once and shared: two states using the same
            // clip at different speeds is a normal thing to author.
            var clipIndex = new Dictionary<AnimationClip, int>();

            foreach (ChildAnimatorState child in machine.states)
            {
                AnimatorState state = child.state;
                if (states.Count >= MaxStates)
                {
                    warnings.Add(
                        $"AnimatorController '{controller.name}' has more than " +
                        $"{MaxStates} states (anim::kMaxStates); '{state.name}' and " +
                        "any after it are dropped.");
                    break;
                }
                AnimationClip clip = ResolveClip(state, controller, warnings);
                if (clip == null)
                    continue;
                if (!clipIndex.TryGetValue(clip, out int ci))
                {
                    if (payload.Clips.Count >= MaxClips)
                    {
                        warnings.Add(
                            $"AnimatorController '{controller.name}' references more " +
                            $"than {MaxClips} clips (anim::kMaxClips); '{clip.name}' " +
                            $"is dropped and state '{state.name}' with it.");
                        continue;
                    }
                    ci = payload.Clips.Count;
                    payload.Clips.Add(P2bAnimExporter.ExportClip(
                        clip, clipRoot, skeleton.Ordered, skeleton.Index,
                        SampleRate, clip.isLooping, PositionTolerance,
                        RotationDotTolerance, ScaleTolerance));
                    clipLengths.Add(clip.length);
                    clipIndex.Add(clip, ci);
                }
                stateIndex[state] = states.Count;
                states.Add(new P2bAnimExporter.StateExport
                {
                    Name = state.name,
                    Clip = ci,
                    Speed = state.speed,
                    Loop = clip.isLooping,
                });
            }

            // The default state has to be index 0: the runtime starts there.
            if (machine.defaultState != null &&
                stateIndex.TryGetValue(machine.defaultState, out int defaultAt) &&
                defaultAt != 0)
            {
                P2bAnimExporter.StateExport first = states[0];
                states[0] = states[defaultAt];
                states[defaultAt] = first;
                foreach (AnimatorState key in new List<AnimatorState>(stateIndex.Keys))
                {
                    if (stateIndex[key] == 0) stateIndex[key] = defaultAt;
                    else if (stateIndex[key] == defaultAt) stateIndex[key] = 0;
                }
            }

            foreach (ChildAnimatorState child in machine.states)
            {
                if (!stateIndex.TryGetValue(child.state, out int from))
                    continue;
                foreach (AnimatorStateTransition t in child.state.transitions)
                {
                    if (transitions.Count >= MaxTransitions)
                    {
                        warnings.Add(
                            $"AnimatorController '{controller.name}' has more than " +
                            $"{MaxTransitions} transitions (anim::kMaxTransitions); " +
                            "the rest are dropped.");
                        break;
                    }
                    if (t.destinationState == null ||
                        !stateIndex.TryGetValue(t.destinationState, out int to))
                    {
                        if (t.destinationStateMachine != null)
                        {
                            warnings.Add(
                                $"Transition from '{child.state.name}' targets a " +
                                "sub-state machine, which is not baked; it is dropped.");
                        }
                        continue;
                    }
                    if (t.conditions.Length > 1)
                    {
                        warnings.Add(
                            $"Transition '{child.state.name}' -> " +
                            $"'{t.destinationState.name}' has {t.conditions.Length} " +
                            "conditions. The runtime evaluates ONE per transition, so " +
                            "only the first is baked -- the transition will fire " +
                            "earlier than it does in the Editor. Split it into " +
                            "several transitions through intermediate states.");
                    }

                    byte condition;
                    byte param = 0;
                    float threshold = 0.0f;
                    if (t.conditions.Length == 0)
                    {
                        if (!t.hasExitTime)
                        {
                            warnings.Add(
                                $"Transition '{child.state.name}' -> " +
                                $"'{t.destinationState.name}' has no conditions and no " +
                                "exit time, so in the Editor it fires immediately. It " +
                                "is baked as exit-time, which fires at the end of the " +
                                "clip instead.");
                        }
                        condition = 0; // kConditionExitTime
                    }
                    else
                    {
                        AnimatorCondition c = t.conditions[0];
                        int pi = parameters.IndexOf(c.parameter);
                        if (pi < 0)
                        {
                            warnings.Add(
                                $"Transition '{child.state.name}' -> " +
                                $"'{t.destinationState.name}' tests parameter " +
                                $"'{c.parameter}', which was not exported; the " +
                                "transition is dropped.");
                            continue;
                        }
                        param = (byte)pi;
                        threshold = c.threshold;
                        switch (c.mode)
                        {
                            case AnimatorConditionMode.If: condition = 1; break;      // BoolTrue
                            case AnimatorConditionMode.IfNot: condition = 2; break;   // BoolFalse
                            case AnimatorConditionMode.Greater: condition = 3; break; // FloatGreater
                            case AnimatorConditionMode.Less: condition = 4; break;    // FloatLess
                            default:
                                // Equals/NotEqual are Int-only in Unity and have no
                                // float-safe equivalent; refusing beats a comparison
                                // that never matches.
                                warnings.Add(
                                    $"Transition '{child.state.name}' -> " +
                                    $"'{t.destinationState.name}' uses condition " +
                                    $"'{c.mode}', which has no equivalent on a float " +
                                    "parameter; the transition is dropped.");
                                continue;
                        }
                        if (IsTrigger(controller, c.parameter))
                            condition = 5; // kConditionTrigger
                    }

                    transitions.Add(new P2bAnimExporter.TransitionExport
                    {
                        From = from,
                        To = to,
                        Duration = t.hasFixedDuration
                                       ? t.duration
                                       : t.duration *
                                             ClipLengthOf(states, clipLengths, to),
                        Condition = condition,
                        Param = param,
                        Threshold = threshold,
                    });
                }
            }
        }

        // Unity's transition duration is NORMALISED to the destination clip
        // unless hasFixedDuration is set; the runtime's is always seconds. So
        // an un-fixed 0.25 means "a quarter of the clip we are going TO", and
        // converting it needs that clip's length -- which is why clip lengths
        // are tracked alongside the exported bytes rather than thrown away.
        private static float ClipLengthOf(List<P2bAnimExporter.StateExport> states,
                                          List<float> clipLengths, int state)
        {
            if (state < 0 || state >= states.Count)
                return 1.0f;
            int clip = states[state].Clip;
            if (clip < 0 || clip >= clipLengths.Count)
                return 1.0f;
            return clipLengths[clip] > 0.0f ? clipLengths[clip] : 1.0f;
        }

        private static bool IsTrigger(AnimatorController controller, string name)
        {
            foreach (AnimatorControllerParameter p in controller.parameters)
            {
                if (p.name == name)
                    return p.type == AnimatorControllerParameterType.Trigger;
            }
            return false;
        }

        private static AnimationClip ResolveClip(AnimatorState state,
                                                 AnimatorController controller,
                                                 List<string> warnings)
        {
            if (state.motion is AnimationClip clip)
                return clip;
            if (state.motion is BlendTree tree)
            {
                // A blend tree's first clip is NOT the blend tree. Say so.
                AnimationClip first = FirstClip(tree);
                warnings.Add(
                    $"AnimatorController '{controller.name}': state '{state.name}' " +
                    "uses a BlendTree, which this runtime cannot evaluate. " +
                    (first != null
                         ? $"Its first clip '{first.name}' is used instead, so the " +
                           "state will play one animation rather than blending."
                         : "It has no clips, so the state is dropped."));
                return first;
            }
            if (state.motion == null)
            {
                warnings.Add(
                    $"AnimatorController '{controller.name}': state '{state.name}' " +
                    "has no motion and is dropped.");
            }
            return null;
        }

        private static AnimationClip FirstClip(BlendTree tree)
        {
            foreach (ChildMotion child in tree.children)
            {
                if (child.motion is AnimationClip clip)
                    return clip;
                if (child.motion is BlendTree nested)
                {
                    AnimationClip found = FirstClip(nested);
                    if (found != null)
                        return found;
                }
            }
            return null;
        }

        // A one-frame empty clip: the bind pose, for a rig with no controller.
        private static AnimationClip StaticPoseClip()
        {
            var clip = new AnimationClip { name = "PS2BindPose" };
            return clip;
        }

        private static Color32 FallbackColour(SkinnedMeshRenderer renderer)
        {
            Material material = renderer.sharedMaterial;
            return material != null ? (Color32)material.color
                                    : new Color32(255, 255, 255, 255);
        }

        private static string PathOf(GameObject go)
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
    }
}
