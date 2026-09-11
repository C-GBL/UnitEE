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
            // NO early return on an empty list: a scene with zero
            // SkinnedMeshRenderers can still be a rigid-bound character, which
            // the block after the usability filter handles.

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

            // Rigid-bound model (M12.5): meshes parented to bones, no skin
            // weights anywhere. Unity animates these by moving the bone
            // TRANSFORMS -- Humanoid with a valid avatar says nothing about
            // skinning, this is exactly the case it describes -- so the
            // export does the same: the skeleton is the Animator's transform
            // hierarchy, and the runtime writes the sampled pose to the
            // matching entities every frame.
            Animator hierarchyAnimator = null;
            if (usable.Count == 0)
            {
                foreach (GameObject root in roots)
                {
                    if (root == null)
                        continue;
                    foreach (Animator a in
                             root.GetComponentsInChildren<Animator>(true))
                    {
                        if (a.runtimeAnimatorController == null)
                            continue;
                        hierarchyAnimator = a;
                        break;
                    }
                    if (hierarchyAnimator != null)
                        break;
                }
                if (hierarchyAnimator == null)
                {
                    if (all.Count > 0)
                        warnings.Add(
                            $"None of the {all.Count} SkinnedMeshRenderers in the " +
                            "scene could be exported, so no character will be " +
                            "drawn. The reasons are above, one per renderer.");
                    return null;
                }
                warnings.Add(
                    $"'{PathOf(hierarchyAnimator.gameObject)}': no usable " +
                    "SkinnedMeshRenderer, so this exports as a TRANSFORM-animated " +
                    "rig: the Animator moves the bone transforms and the rigid " +
                    "meshes riding them. Bones bind to entities by NAME, so " +
                    "renaming a bone after export breaks its binding.");
            }

            // Two or more distinct Animators each driving skinned renderers
            // are separate characters: each gets its OWN skeleton, its own
            // skinned meshes in its own space, and its own animator; the
            // controller and clips are shared (same rig, same bone order).
            // The single-character path below is left exactly as it was, so
            // a lone character exports byte-for-byte as before.
            if (usable.Count > 0)
            {
                var distinctAnimators = new HashSet<Transform>();
                foreach (SkinnedMeshRenderer smr in usable)
                {
                    Animator a = smr.GetComponentInParent<Animator>();
                    distinctAnimators.Add(a != null ? a.transform : smr.transform.root);
                }
                if (distinctAnimators.Count > 1)
                {
                    return BakeMultiCharacter(usable, warnings);
                }
            }

            Animator animator = usable.Count > 0
                ? usable[0].GetComponentInParent<Animator>()
                : hierarchyAnimator;
            // The rig's runtime space: the entity the renderer records bind
            // to, and the space every rest transform, bind matrix, VERTEX
            // and clip key is exported in.
            Transform reference = animator != null ? animator.transform
                                : usable.Count > 0 ? usable[0].transform.root
                                : hierarchyAnimator.transform;

            // ONE skeleton, shared. Every usable renderer contributes its bones
            // to a union: this is how a character is actually authored, with
            // each material a separate renderer over the same rig.
            var unionBones = new List<Transform>();
            var unionBind = new List<Matrix4x4>();
            var boneAt = new Dictionary<Transform, int>();
            foreach (SkinnedMeshRenderer smr in usable)
            {
                Transform[] bones = smr.bones;
                // Bind poses are REFERENCE-relative and therefore identical
                // for every renderer that shares a bone -- which the union
                // skeleton requires. Unity's stored bindposes map from each
                // renderer's OWN mesh space, and a character's renderers sit
                // at different node transforms (this model's arms at y 1.54,
                // body at -0.02, head at 1.89): one stored bind per bone put
                // the body's vertices through the arms' map, 1.56 units up --
                // "the body is above the head", literally (verify-log
                // M12.5). The vertices are baked into reference space at
                // export to match. Requires the scene pose to be the bind
                // pose, which an imported character in its default pose
                // satisfies.
                for (int i = 0; i < bones.Length; i++)
                {
                    Matrix4x4 bind = bones[i].worldToLocalMatrix *
                                     reference.localToWorldMatrix;
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

            if (hierarchyAnimator != null)
            {
                CollectRigidBones(hierarchyAnimator.transform, unionBones,
                                  unionBind, boneAt);
                var seen = new HashSet<string>();
                foreach (Transform bone in unionBones)
                {
                    if (!seen.Add(bone.name))
                        warnings.Add(
                            $"'{PathOf(hierarchyAnimator.gameObject)}': two bones " +
                            $"named '{bone.name}'. Entity binding is by name, so " +
                            "one of them will take the other's animation.");
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

            var payload = new P2bSceneExporter.SkinPayload();

            P2bAnimExporter.SkeletonExport skeleton = P2bAnimExporter.ExportSkeleton(
                unionBones.ToArray(), unionBind.ToArray(), reference);
            payload.Skeletons.Add(skeleton.Bytes);
            if (usable.Count > 0)
            {
                BuildDiagnosis(payload, usable, reference);
            }

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
            var trees = new List<P2bAnimExporter.BlendTreeExport>();
            var parameters = new List<string>();
            var stateIndex = new Dictionary<AnimatorState, int>();
            var clipLengths = new List<float>();

            if (controller != null)
            {
                ReadController(controller, clipRoot, skeleton, payload, states,
                               transitions, parameters, stateIndex, clipLengths,
                               trees, warnings);
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
                        skeleton.Index, skeleton.RestRef, SampleRate, false,
                        PositionTolerance, RotationDotTolerance,
                        ScaleTolerance, null, skeleton.RestPos,
                        skeleton.RestRot));
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
                states.ToArray(), transitions.ToArray(), parameters.ToArray(),
                trees);

            // One SKMS per distinct (mesh, submesh). A renderer's submeshes
            // are its material slots -- a body mesh carries its teeth and
            // lashes as further slots -- so each gets its own section with
            // its own colour and texture; exporting mesh.triangles under
            // slot 0 drew every slot in the body's colour. Renderers sharing
            // a mesh share the entries, so a crowd of one character costs
            // one set of meshes.
            var meshAt = new Dictionary<(Mesh, int), int>();
            var groupAt = new Dictionary<Transform, int>();
            foreach (SkinnedMeshRenderer smr in usable)
            {
                Mesh mesh = smr.sharedMesh;
                Material[] mats = smr.sharedMaterials;
                // Bakes the renderer's node transform into the vertices:
                // every mesh lands in reference space, matching the
                // reference-relative binds above.
                Matrix4x4 toReference = reference.worldToLocalMatrix *
                                        smr.transform.localToWorldMatrix;
                var drawn = new List<int>();
                int slots = Mathf.Max(1, mesh.subMeshCount);
                for (int s = 0; s < slots; s++)
                {
                    if (mesh.subMeshCount > 0 && mesh.GetSubMesh(s).indexCount == 0)
                        continue; // the runtime refuses a zero-batch mesh
                    int at;
                    if (!meshAt.TryGetValue((mesh, s), out at))
                    {
                        // Unity's slot rule: material s, or the last one
                        // when the renderer has fewer materials than slots.
                        Material mat = null;
                        if (mats.Length > 0)
                        {
                            mat = s < mats.Length ? mats[s] : mats[mats.Length - 1];
                            if (mat == null) mat = mats[0];
                        }
                        // The texture is the slot's main texture; without
                        // one (or without UVs) the mesh exports in the
                        // 5-qword vertex-coloured format the M9 rigs use.
                        Texture2D tex = mat != null
                            ? mat.mainTexture as Texture2D : null;
                        bool textured = tex != null && mesh.uv != null &&
                                        mesh.uv.Length == mesh.vertexCount;
                        at = payload.SkinnedMeshes.Count;
                        payload.SkinnedMeshes.Add(P2bAnimExporter.ExportSkinnedMesh(
                            mesh, 0, FallbackColour(mat),
                            skeleton.Ordered, skeleton.Index, smr.bones, 0,
                            textured, toReference, toReference,
                            mesh.subMeshCount > 1 ? s : -1,
                            /*preferVertexColours=*/ mat == null));
                        payload.MeshTextures.Add(textured ? tex : null);
                        meshAt[(mesh, s)] = at;
                    }
                    drawn.Add(at);
                }
                if (drawn.Count == 0)
                {
                    warnings.Add(
                        $"{PathOf(smr.gameObject)}: mesh '{mesh.name}' has no " +
                        "triangles in any submesh, so nothing is exported for it.");
                    continue;
                }
                payload.RendererMesh[smr] = drawn[0];
                payload.RendererMeshes[smr] = drawn;

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
                    payload.GroupAnimators.Add(key);
                    if (group > 0)
                        warnings.Add(
                            $"'{key.name}': a second animated character in " +
                            "one scene shares the first one's skeleton " +
                            "reference space; if their transforms differ, " +
                            "this one may sit or scale wrong.");
                }
                payload.RendererGroup[smr] = group;
            }
            return payload;
        }

        // Several characters, each its own skeleton (verify-log 2026-09-11).
        // Renderers are grouped by their Animator; every group builds a
        // skeleton from ITS OWN bones relative to ITS OWN animator, and
        // exports ITS OWN skinned meshes to that space with that skeleton
        // index -- so nothing about one character's transform, scale or
        // pose reaches another's. The controller and its clips are built
        // once, from the first group, and shared: the characters are the
        // same rig, so every skeleton has the same bones in the same order,
        // and a clip's per-bone-INDEX tracks apply correctly to each.
        private static P2bSceneExporter.SkinPayload BakeMultiCharacter(
            List<SkinnedMeshRenderer> usable, List<string> warnings)
        {
            var groupsInOrder = new List<Transform>();
            var byGroup = new Dictionary<Transform, List<SkinnedMeshRenderer>>();
            foreach (SkinnedMeshRenderer smr in usable)
            {
                Animator owner = smr.GetComponentInParent<Animator>();
                Transform key = owner != null ? owner.transform : smr.transform.root;
                if (!byGroup.TryGetValue(key, out List<SkinnedMeshRenderer> list))
                {
                    list = new List<SkinnedMeshRenderer>();
                    byGroup[key] = list;
                    groupsInOrder.Add(key);
                }
                list.Add(smr);
            }

            var payload = new P2bSceneExporter.SkinPayload();
            P2bAnimExporter.SkeletonExport firstSkeleton = null;
            GameObject firstClipRoot = null;
            Animator firstController = null;
            int firstBoneCount = -1;

            for (int g = 0; g < groupsInOrder.Count; g++)
            {
                Transform key = groupsInOrder[g];
                Animator groupAnimator = key.GetComponent<Animator>();
                Transform reference = groupAnimator != null ? groupAnimator.transform : key;
                List<SkinnedMeshRenderer> rends = byGroup[key];

                // This group's skeleton, from this group's bones, relative to
                // this group's reference -- the same union as the single
                // character path, but scoped to one animator.
                var unionBones = new List<Transform>();
                var unionBind = new List<Matrix4x4>();
                var boneAt = new Dictionary<Transform, int>();
                foreach (SkinnedMeshRenderer smr in rends)
                {
                    Transform[] bones = smr.bones;
                    for (int i = 0; i < bones.Length; i++)
                    {
                        Matrix4x4 bind = bones[i].worldToLocalMatrix *
                                         reference.localToWorldMatrix;
                        if (boneAt.TryGetValue(bones[i], out int at))
                        {
                            if (!BindposesAgree(unionBind[at], bind))
                                warnings.Add(
                                    $"{PathOf(smr.gameObject)}: bone '{bones[i].name}' has a " +
                                    "different bind pose than on an earlier renderer of the " +
                                    "same character; this mesh may be skinned off its axis.");
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
                        $"'{PathOf(key.gameObject)}': the character needs {unionBones.Count} " +
                        $"bones, over the runtime's limit of {MaxBones}. It is not exported.");
                    return null;
                }

                P2bAnimExporter.SkeletonExport skeleton = P2bAnimExporter.ExportSkeleton(
                    unionBones.ToArray(), unionBind.ToArray(), reference);
                payload.Skeletons.Add(skeleton.Bytes);

                if (g == 0)
                {
                    firstSkeleton = skeleton;
                    firstController = groupAnimator;
                    firstClipRoot = groupAnimator != null ? groupAnimator.gameObject
                                                          : rends[0].gameObject;
                    firstBoneCount = unionBones.Count;
                    BuildDiagnosis(payload, rends, reference);
                }
                else if (unionBones.Count != firstBoneCount)
                {
                    warnings.Add(
                        $"'{PathOf(key.gameObject)}': this character has {unionBones.Count} " +
                        $"bones but the first has {firstBoneCount}. They share one controller, " +
                        "so its clips may drive the wrong bones here; give each its own rig " +
                        "or make them identical imports.");
                }

                // This group's skinned meshes, deduped WITHIN the group, in
                // this group's space and skeleton index.
                var meshAt = new Dictionary<(Mesh, int), int>();
                foreach (SkinnedMeshRenderer smr in rends)
                {
                    Mesh mesh = smr.sharedMesh;
                    Material[] mats = smr.sharedMaterials;
                    Matrix4x4 toReference = reference.worldToLocalMatrix *
                                            smr.transform.localToWorldMatrix;
                    var drawn = new List<int>();
                    int slots = Mathf.Max(1, mesh.subMeshCount);
                    for (int s = 0; s < slots; s++)
                    {
                        if (mesh.subMeshCount > 0 && mesh.GetSubMesh(s).indexCount == 0)
                            continue;
                        if (!meshAt.TryGetValue((mesh, s), out int at))
                        {
                            Material mat = null;
                            if (mats.Length > 0)
                            {
                                mat = s < mats.Length ? mats[s] : mats[mats.Length - 1];
                                if (mat == null) mat = mats[0];
                            }
                            Texture2D tex = mat != null ? mat.mainTexture as Texture2D : null;
                            bool textured = tex != null && mesh.uv != null &&
                                            mesh.uv.Length == mesh.vertexCount;
                            at = payload.SkinnedMeshes.Count;
                            payload.SkinnedMeshes.Add(P2bAnimExporter.ExportSkinnedMesh(
                                mesh, 0, FallbackColour(mat),
                                skeleton.Ordered, skeleton.Index, smr.bones, g,
                                textured, toReference, toReference,
                                mesh.subMeshCount > 1 ? s : -1,
                                /*preferVertexColours=*/ mat == null));
                            payload.MeshTextures.Add(textured ? tex : null);
                            meshAt[(mesh, s)] = at;
                        }
                        drawn.Add(at);
                    }
                    if (drawn.Count == 0)
                    {
                        warnings.Add(
                            $"{PathOf(smr.gameObject)}: mesh '{mesh.name}' has no triangles; " +
                            "nothing exported for it.");
                        continue;
                    }
                    payload.RendererMesh[smr] = drawn[0];
                    payload.RendererMeshes[smr] = drawn;
                    payload.RendererGroup[smr] = g;
                }
                payload.GroupAnimators.Add(reference);
            }

            // The controller and clips, once, from the first character.
            if (payload.SkinnedMeshes.Count > kMaxMultiSkinnedMeshes)
            {
                warnings.Add(
                    $"{payload.SkinnedMeshes.Count} skinned meshes across " +
                    $"{groupsInOrder.Count} characters, over the runtime's {kMaxMultiSkinnedMeshes}; " +
                    "the extra ones will not load. Use fewer characters or fewer materials.");
            }
            var controller = firstController != null
                ? firstController.runtimeAnimatorController as AnimatorController : null;
            var states = new List<P2bAnimExporter.StateExport>();
            var transitions = new List<P2bAnimExporter.TransitionExport>();
            var trees = new List<P2bAnimExporter.BlendTreeExport>();
            var parameters = new List<string>();
            var stateIndex = new Dictionary<AnimatorState, int>();
            var clipLengths = new List<float>();
            if (controller != null)
            {
                ReadController(controller, firstClipRoot, firstSkeleton, payload, states,
                               transitions, parameters, stateIndex, clipLengths,
                               trees, warnings);
            }
            if (states.Count == 0)
            {
                if (payload.Clips.Count == 0)
                {
                    payload.Clips.Add(P2bAnimExporter.ExportClip(
                        StaticPoseClip(), firstClipRoot, firstSkeleton.Ordered,
                        firstSkeleton.Index, firstSkeleton.RestRef, SampleRate, false,
                        PositionTolerance, RotationDotTolerance, ScaleTolerance, null,
                        firstSkeleton.RestPos, firstSkeleton.RestRot));
                    clipLengths.Add(0.0f);
                }
                states.Add(new P2bAnimExporter.StateExport
                {
                    Name = "Default", Clip = 0, Speed = 1.0f, Loop = true,
                });
            }
            payload.Controller = P2bAnimExporter.ExportController(
                states.ToArray(), transitions.ToArray(), parameters.ToArray(), trees);
            return payload;
        }

        // The runtime's kMaxSkinnedRenderers ceiling on distinct SKMS.
        private const int kMaxMultiSkinnedMeshes = 24;

        // Measurements, not inferences: Unity's own matrices for the rig
        // as it stands, plus one vertex pushed through BOTH skinning
        // formulas -- Unity's (boneWorld x storedBind) and this exporter's
        // (rest chain x recomputed bind). All positions are relative to the
        // reference (Animator) transform, the space the runtime works in.
        private static void BuildDiagnosis(P2bSceneExporter.SkinPayload payload,
                                           List<SkinnedMeshRenderer> usable,
                                           Transform reference)
        {
            var d = payload.Diagnosis;
            d.Add("{ \"reference\": \"" + PathOf(reference.gameObject) + "\",");
            d.Add("  \"referenceLossyScale\": \"" + reference.lossyScale + "\",");
            d.Add("  \"renderers\": [");
            foreach (SkinnedMeshRenderer smr in usable)
            {
                Mesh mesh = smr.sharedMesh;
                Matrix4x4 refW2L = reference.worldToLocalMatrix;
                Matrix4x4 rel = refW2L * smr.transform.localToWorldMatrix;
                Matrix4x4[] stored = mesh.bindposes;
                Transform bone0 = smr.bones[0];
                Matrix4x4 boneRel = refW2L * bone0.localToWorldMatrix;
                Matrix4x4 recomputed0 = bone0.worldToLocalMatrix *
                                        smr.transform.localToWorldMatrix;

                Vector3 v0 = mesh.vertices[0];
                BoneWeight bw = mesh.boneWeights.Length > 0
                                    ? mesh.boneWeights[0]
                                    : default(BoneWeight);
                Transform strong = smr.bones[bw.boneIndex0];
                Matrix4x4 storedStrong = bw.boneIndex0 < stored.Length
                                             ? stored[bw.boneIndex0]
                                             : Matrix4x4.identity;
                Vector3 unityV0 = (refW2L * strong.localToWorldMatrix *
                                   storedStrong).MultiplyPoint3x4(v0);
                Vector3 oursV0 = rel.MultiplyPoint3x4(v0);

                d.Add("    { \"smr\": \"" + PathOf(smr.gameObject) + "\",");
                d.Add("      \"mesh\": \"" + mesh.name + "\", \"subMeshes\": " +
                      mesh.subMeshCount + ", \"verts\": " + mesh.vertexCount + ",");
                d.Add("      \"smrRelPos\": \"" + (Vector3)rel.GetColumn(3) +
                      "\", \"smrRelScale\": \"" + rel.lossyScale + "\",");
                d.Add("      \"bone0\": \"" + bone0.name +
                      "\", \"bone0RelPos\": \"" + (Vector3)boneRel.GetColumn(3) +
                      "\", \"bone0RelScale\": \"" + boneRel.lossyScale + "\",");
                d.Add("      \"storedBind0Scale\": \"" +
                      (stored.Length > 0 ? stored[0].lossyScale : Vector3.zero) +
                      "\", \"recomputedBind0Scale\": \"" +
                      recomputed0.lossyScale + "\",");
                d.Add("      \"meshBoundsCenter\": \"" + mesh.bounds.center +
                      "\", \"meshBoundsExtents\": \"" + mesh.bounds.extents + "\",");
                d.Add("      \"strongBone\": \"" + strong.name +
                      "\", \"vert0UnityTruth\": \"" + unityV0 +
                      "\", \"vert0OurPipeline\": \"" + oursV0 + "\" },");
            }
            d.Add("  ] }");
        }

        // Strict descendants of the Animator's transform, parents before
        // children -- the animator root itself stays out, so the scene's
        // placement of the character is untouched by the pose. Bind poses are
        // identity: nothing is skinned, the pose IS the transform.
        private static void CollectRigidBones(Transform parent,
                                              List<Transform> bones,
                                              List<Matrix4x4> binds,
                                              Dictionary<Transform, int> boneAt)
        {
            for (int i = 0; i < parent.childCount; i++)
            {
                Transform child = parent.GetChild(i);
                if (!boneAt.ContainsKey(child))
                {
                    boneAt[child] = bones.Count;
                    bones.Add(child);
                    binds.Add(Matrix4x4.identity);
                }
                CollectRigidBones(child, bones, binds, boneAt);
            }
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
            List<float> clipLengths,
            List<P2bAnimExporter.BlendTreeExport> trees, List<string> warnings)
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
                // A 1D blend tree of plain clips becomes a real blend-tree
                // state: every child clip exports, the runtime blends the
                // bracketing pair by the tree's parameter. Anything else
                // (2D, nested, missing param) falls through to ResolveClip's
                // first-clip degradation with its warning.
                P2bAnimExporter.BlendTreeExport treeExport =
                    TryReadBlendTree(state, controller, clipRoot, skeleton,
                                     payload, parameters, clipIndex,
                                     clipLengths, warnings);
                if (treeExport != null)
                {
                    treeExport.State = states.Count;
                    trees.Add(treeExport);
                    stateIndex[state] = states.Count;
                    states.Add(new P2bAnimExporter.StateExport
                    {
                        Name = state.name,
                        Clip = treeExport.Children[0].clip,
                        Speed = state.speed,
                        Loop = true, // locomotion trees cycle
                    });
                    continue;
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
                        skeleton.RestRef, SampleRate, clip.isLooping,
                        PositionTolerance, RotationDotTolerance,
                        ScaleTolerance, warnings, skeleton.RestPos,
                        skeleton.RestRot));
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
                // Blend trees name states by index; the swap moves them too.
                foreach (P2bAnimExporter.BlendTreeExport tree in trees)
                {
                    if (tree.State == 0) tree.State = defaultAt;
                    else if (tree.State == defaultAt) tree.State = 0;
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

        // Reads a state's motion as a bakeable 1D blend tree: Simple1D, at
        // least two DIRECT AnimationClip children, and a parameter the
        // controller exports. Returns null (leaving the ResolveClip
        // degradation to run) for anything else. State index is filled by
        // the caller.
        private static P2bAnimExporter.BlendTreeExport TryReadBlendTree(
            AnimatorState state, AnimatorController controller,
            GameObject clipRoot, P2bAnimExporter.SkeletonExport skeleton,
            P2bSceneExporter.SkinPayload payload, List<string> parameters,
            Dictionary<AnimationClip, int> clipIndex, List<float> clipLengths,
            List<string> warnings)
        {
            if (!(state.motion is BlendTree tree))
                return null;
            if (tree.blendType != BlendTreeType.Simple1D)
                return null; // ResolveClip warns and takes the first clip
            int paramAt = parameters.IndexOf(tree.blendParameter);
            if (paramAt < 0)
            {
                warnings.Add(
                    $"BlendTree '{tree.name}' blends on '{tree.blendParameter}', " +
                    "which is not among the exported parameters; its first clip " +
                    "plays instead.");
                return null;
            }
            var children = new List<(int clip, float threshold)>();
            foreach (ChildMotion child in tree.children)
            {
                // A NESTED 1D tree (the stock locomotion shape: Speed blends
                // Walks/Runs, each blending turn variants by Direction)
                // flattens to the nested child at the inner parameter's
                // DEFAULT value -- the straight-ahead clip. The outer axis
                // survives exactly; the inner lean variants are dropped
                // with a warning.
                Motion motion = child.motion;
                while (motion is BlendTree nested)
                {
                    if (nested.blendType != BlendTreeType.Simple1D ||
                        nested.children.Length == 0)
                        return null;
                    float def = 0.0f;
                    foreach (AnimatorControllerParameter p in
                             controller.parameters)
                    {
                        if (p.name == nested.blendParameter)
                        {
                            def = p.defaultFloat;
                            break;
                        }
                    }
                    ChildMotion best = nested.children[0];
                    foreach (ChildMotion c in nested.children)
                    {
                        if (Mathf.Abs(c.threshold - def) <
                            Mathf.Abs(best.threshold - def))
                            best = c;
                    }
                    warnings.Add(
                        $"BlendTree '{tree.name}': nested tree " +
                        $"'{nested.name}' flattened to its child at " +
                        $"{nested.blendParameter} = {def} " +
                        $"('{(best.motion != null ? best.motion.name : "?")}'); " +
                        "the inner blend axis is not modelled.");
                    motion = best.motion;
                }
                if (!(motion is AnimationClip clip))
                    return null; // no clip at the bottom: first-clip fallback
                if (children.Count >= (int)Ps2BlendChildCap)
                {
                    warnings.Add(
                        $"BlendTree '{tree.name}' has more than " +
                        $"{Ps2BlendChildCap} children (anim::kMaxBlendChildren); " +
                        "the rest are dropped.");
                    break;
                }
                if (!clipIndex.TryGetValue(clip, out int ci))
                {
                    if (payload.Clips.Count >= MaxClips)
                    {
                        warnings.Add(
                            $"AnimatorController '{controller.name}' references " +
                            $"more than {MaxClips} clips; BlendTree '{tree.name}' " +
                            "cannot bake fully and plays its first clip.");
                        return null;
                    }
                    ci = payload.Clips.Count;
                    payload.Clips.Add(P2bAnimExporter.ExportClip(
                        clip, clipRoot, skeleton.Ordered, skeleton.Index,
                        skeleton.RestRef, SampleRate, true,
                        PositionTolerance, RotationDotTolerance,
                        ScaleTolerance, warnings, skeleton.RestPos,
                        skeleton.RestRot));
                    clipLengths.Add(clip.length);
                    clipIndex.Add(clip, ci);
                }
                children.Add((ci, child.threshold));
            }
            if (children.Count < 2)
                return null;
            children.Sort((x, y) => x.threshold.CompareTo(y.threshold));
            var result = new P2bAnimExporter.BlendTreeExport { Param = paramAt };
            result.Children.AddRange(children);
            return result;
        }

        private const uint Ps2BlendChildCap = 6; // anim::kMaxBlendChildren

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

        private static Color32 FallbackColour(Material material)
        {
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
