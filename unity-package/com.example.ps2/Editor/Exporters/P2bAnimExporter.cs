// Skeletons, clips, controllers and skinned meshes -> .p2b (plan section 9,
// M9 tasks 1 and 3; layouts in docs/formats/p2b-container.md).
//
// Three things here are algorithms rather than serialisation:
//
//  1. Bone ordering. The runtime resolves bone matrices in ONE linear pass,
//     so bones must be written parent-before-child. Unity's bones[] array
//     carries no such guarantee, so this topologically sorts and permutes
//     the bind poses and every weight index to match.
//  2. Keyframe reduction. Clips are sampled on a fixed grid and then thinned
//     greedily: a sample survives only if dropping it would move the
//     reconstructed curve further than the tolerance at some point in
//     between. Rotations use the quaternion dot product, not per-component
//     error, because components are not independent.
//  3. Bone partitioning (M9 task 3). Batches may reference at most 24 bones.
//     This walks triangles in order, accumulating a batch until adding the
//     next triangle would overflow the palette or the vertex budget -- the
//     greedy rule specified and unit-tested in runtime/src/anim (the runtime
//     re-validates every table it loads, so a disagreement fails loudly).
using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Animations;
using UnityEngine.Playables;

namespace Ps2.Editor
{
    internal static class P2bAnimExporter
    {
        public const int MaxPaletteBones = 24;
        // 5 qwords per skinned vertex; the VIF NUM field is 8 bits, so a
        // batch unpack stays at or under 255 qwords (48 * 5 = 240).
        public const int MaxSkinVertsPerBatch = 48;
        public const uint SkinVertDest = 114;

        public sealed class SkeletonExport
        {
            public byte[] Bytes;
            public Transform[] Ordered;              // parent-before-child
            public Dictionary<Transform, int> Index; // bone -> new index
            // The transform each bone's rest (and every clip key) is
            // RELATIVE TO: its nearest bone ancestor, or the reference for
            // roots. Never the raw Unity parent -- an FBX routinely parks
            // scaled non-bone nodes (the armature) between the Animator and
            // the rig, and raw locals silently drop them, which put a whole
            // character behind the near plane (verify-log M12.5).
            public Transform[] RestRef;
            // The rest pose in RestRef space, captured ONCE from the scene
            // (which must be posed at the bind pose when the rig exports).
            // Clip export takes its bind reference from here: sampling
            // leaves the scene transforms at the last sampled frame, so a
            // capture made per clip saw the PREVIOUS clip's final pose --
            // a turn clip centred on the walk's last step, 2 units off.
            public Vector3[] RestPos;
            public Quaternion[] RestRot;
        }

        // Position/rotation/scale out of a TRS matrix. Exact for the
        // uniform-ish scales rigs actually carry.
        private static void Decompose(Matrix4x4 m, out Vector3 pos,
                                      out Quaternion rot, out Vector3 scale)
        {
            pos = new Vector3(m.m03, m.m13, m.m23);
            rot = m.rotation;
            scale = m.lossyScale;
        }

        // ---- skeleton ------------------------------------------------------

        // 'reference' is the space the whole rig lives in at runtime: the
        // entity whose world matrix the renderer multiplies the palette by
        // (the Animator's transform). Root bones are exported relative to
        // it, composing THROUGH any non-bone nodes in between.
        public static SkeletonExport ExportSkeleton(Transform[] bones,
                                                    Matrix4x4[] bindposes,
                                                    Transform reference)
        {
            var known = new HashSet<Transform>(bones);
            var ordered = new List<Transform>(bones.Length);
            var placed = new HashSet<Transform>();
            // Topological order: a bone becomes eligible once its nearest
            // ancestor that is also a bone has been emitted.
            while (ordered.Count < bones.Length)
            {
                bool progress = false;
                foreach (Transform bone in bones)
                {
                    if (placed.Contains(bone)) continue;
                    Transform parent = NearestBoneAncestor(bone, known);
                    if (parent == null || placed.Contains(parent))
                    {
                        ordered.Add(bone);
                        placed.Add(bone);
                        progress = true;
                    }
                }
                if (!progress)
                {
                    throw new InvalidOperationException(
                        "[PS2] bone hierarchy has a cycle; cannot order parents first");
                }
            }

            var index = new Dictionary<Transform, int>();
            for (int i = 0; i < ordered.Count; i++)
            {
                index[ordered[i]] = i;
            }

            var restRef = new Transform[ordered.Count];

            var restPos = new Vector3[ordered.Count];

            var restRot = new Quaternion[ordered.Count];
            var b = new ByteBuffer();
            b.U32((uint)ordered.Count);
            b.U32(0);
            b.U32(0);
            b.U32(0);
            for (int i = 0; i < ordered.Count; i++)
            {
                Transform bone = ordered[i];
                Transform parent = NearestBoneAncestor(bone, known);
                b.I32(parent != null ? index[parent] : -1);
                b.U32((uint)P2bWriter.Fnv1a64(bone.name));
                // Bind poses are indexed by the ORIGINAL bones[] order.
                int original = Array.IndexOf(bones, bone);
                Matrix4x4 bind = bindposes[original];
                for (int m = 0; m < 16; m++)
                {
                    b.F32(bind[m]);
                }
                // Rest RELATIVE TO the runtime parent, not the Unity parent:
                // they differ whenever a non-bone node (a scaled armature, a
                // grouping null) sits between them, and its transform must
                // fold in here or vanish from the character entirely.
                Transform refT = parent != null ? parent : reference;
                restRef[i] = refT;
                Matrix4x4 rel = refT != null
                    ? refT.worldToLocalMatrix * bone.localToWorldMatrix
                    : bone.localToWorldMatrix;
                Decompose(rel, out Vector3 rp, out Quaternion rr,
                          out Vector3 rs);
                restPos[i] = rp;
                restRot[i] = rr;
                b.F32(rp.x);
                b.F32(rp.y);
                b.F32(rp.z);
                b.F32(rr.x);
                b.F32(rr.y);
                b.F32(rr.z);
                b.F32(rr.w);
                b.F32(rs.x);
                b.F32(rs.y);
                b.F32(rs.z);
            }
            return new SkeletonExport
            {
                Bytes = b.ToArray(),
                Ordered = ordered.ToArray(),
                Index = index,
                RestRef = restRef,
                RestPos = restPos,
                RestRot = restRot,
            };
        }

        private static Transform NearestBoneAncestor(Transform bone,
                                                     HashSet<Transform> bones)
        {
            for (Transform p = bone.parent; p != null; p = p.parent)
            {
                if (bones.Contains(p)) return p;
            }
            return null;
        }

        // ---- clips ---------------------------------------------------------

        private struct SampledTrack
        {
            public int Bone;
            public Vector3[] Position;
            public Quaternion[] Rotation;
            public Vector3[] Scale;
        }

        // Samples the clip on a fixed grid by driving Unity's own sampler,
        // then reduces and quantises. sampleRate is in Hz.
        public static byte[] ExportClip(AnimationClip clip, GameObject root,
                                        Transform[] ordered,
                                        Dictionary<Transform, int> index,
                                        Transform[] restRef,
                                        float sampleRate, bool loop,
                                        float positionTolerance,
                                        float rotationDotTolerance,
                                        float scaleTolerance,
                                        List<string> warnings = null,
                                        Vector3[] restPos = null,
                                        Quaternion[] restRot = null)
        {
            int sampleCount = Mathf.Max(2, Mathf.RoundToInt(clip.length * sampleRate) + 1);
            // An active Animation window / Timeline preview (AnimationMode)
            // pins every previewed transform at the PREVIEW pose: the bake
            // graph writes a sample and the pin writes it straight back, so
            // bones read back frozen -- and Rebind cannot clear it, only
            // stopping the preview can. A build is allowed to.
            if (UnityEditor.AnimationMode.InAnimationMode())
            {
                UnityEditor.AnimationMode.StopAnimationMode();
                if (warnings != null)
                {
                    warnings.Add(
                        "an Animation window / Timeline preview was active; " +
                        "the exporter stopped it (an active preview pins " +
                        "bones at the preview pose and corrupts clip " +
                        "sampling).");
                }
            }
            // The scene pose IS the bind pose when clips are exported (the
            // rig exporter requires it); captured before sampling moves
            // anything, it is the reference the partial-freeze guard below
            // compares against.
            // Preferably the skeleton's own capture (see SkeletonExport):
            // by the second clip the scene is posed at the previous clip's
            // last sample, not the bind pose.
            var bindRot = new Quaternion[ordered.Length];
            var bindPos = new Vector3[ordered.Length];
            for (int i = 0; i < ordered.Length; i++)
            {
                if (restPos != null && restRot != null &&
                    i < restPos.Length && i < restRot.Length)
                {
                    bindPos[i] = restPos[i];
                    bindRot[i] = restRot[i];
                    continue;
                }
                Transform refT0 = restRef != null ? restRef[i] : null;
                bindRot[i] = refT0 != null
                    ? Quaternion.Inverse(refT0.rotation) * ordered[i].rotation
                    : ordered[i].localRotation;
                bindPos[i] = refT0 != null
                    ? refT0.InverseTransformPoint(ordered[i].position)
                    : ordered[i].localPosition;
            }
            var tracks = new SampledTrack[ordered.Length];
            for (int i = 0; i < ordered.Length; i++)
            {
                tracks[i].Bone = i;
                tracks[i].Position = new Vector3[sampleCount];
                tracks[i].Rotation = new Quaternion[sampleCount];
                tracks[i].Scale = new Vector3[sampleCount];
            }

            // TWO samplers, because humanoid and generic motion are stored
            // differently. A humanoid clip keeps the BODY in muscle curves
            // that exist only through avatar retargeting; SampleAnimation
            // evaluates transform curves directly and silently skips them,
            // so the body freezes in its rest pose while the generic bones
            // riding the same clip -- hair, ribbons, cloth -- animate.
            // Exactly half a character, and nothing says why (verify-log
            // M12.5). A PlayableGraph driven through the Animator is the
            // sampler that retargets.
            Animator animator = root.GetComponent<Animator>();
            bool viaGraph = animator != null && animator.isHuman &&
                            clip.isHumanMotion;
            // The graph writes its pose THROUGH the Animator, and the
            // Animator honours its culling mode even for a manual Evaluate.
            // A headless build renders nothing, so with Cull Update
            // Transforms (the scene default for many characters) every
            // renderer counts as invisible and the evaluate is a no-op:
            // 24 clips exported, every track a constant at the rest pose,
            // and nothing said why (verify-log M12.5). Baking always
            // animates; culling is a runtime concern.
            AnimatorCullingMode wasCulling = animator != null
                ? animator.cullingMode : AnimatorCullingMode.AlwaysAnimate;
            if (animator != null)
            {
                animator.cullingMode = AnimatorCullingMode.AlwaysAnimate;
                // A long-running Editor session can leave the Animator's
                // internal bindings STALE -- an asset reimport under the
                // open scene is enough -- and a graph evaluated through
                // stale bindings poses only part of the skeleton: most
                // tracks sample as the bind pose and the character ships
                // half frozen, with nothing failing. Rebinding is cheap
                // and makes sampling immune to session history.
                animator.Rebind();
            }
            // The motion node is usually NOT a sampled bone (the skinned
            // skeleton starts at the pelvis; "root" sits above it), so its
            // transform is recorded alongside the bones and its unbaked
            // channels are taken back out of every top-level bone below it
            // afterwards (StripRootMotion).
            Transform motionNode = FindMotionNode(root, animator, ordered);
            var nodePos = new Vector3[sampleCount];
            var nodeRot = new Quaternion[sampleCount];
            var nodeScale = new Vector3[sampleCount];
            SampleTracks(clip, root, ordered, restRef, animator, viaGraph,
                         sampleCount, tracks, motionNode, nodePos, nodeRot,
                         nodeScale);

            // The partial-freeze signature: most rotation tracks pinned at
            // the bind pose for the whole clip. Rebind alone has been seen
            // NOT to cure it -- a session can hold the freeze in the
            // imported avatar/clip objects themselves -- so when it shows
            // up the exporter repairs the session the way a human would,
            // with a forced synchronous reimport of the assets involved,
            // and samples again, instead of shipping a frozen character
            // under a warning nobody reads.
            bool frozen = viaGraph && ordered.Length > 0 &&
                          clip.length > 0.05f &&
                          CountRotationTracksAtBind(tracks, bindRot,
                                                    sampleCount) * 100 >=
                              ordered.Length * FreezeThresholdPercent;
            bool healed = false;
            if (frozen)
            {
                string clipPath = UnityEditor.AssetDatabase.GetAssetPath(clip);
                string clipName = clip.name;
                string avatarPath = animator.avatar != null
                    ? UnityEditor.AssetDatabase.GetAssetPath(animator.avatar)
                    : null;
                var paths = new List<string>();
                if (!string.IsNullOrEmpty(avatarPath)) paths.Add(avatarPath);
                if (!string.IsNullOrEmpty(clipPath) && clipPath != avatarPath)
                {
                    paths.Add(clipPath);
                }
                bool attempted = false;
                foreach (string p in paths)
                {
                    if (s_healFailed.Contains(p)) continue;
                    UnityEditor.AssetDatabase.ImportAsset(
                        p, UnityEditor.ImportAssetOptions.ForceUpdate |
                           UnityEditor.ImportAssetOptions.ForceSynchronousImport);
                    attempted = true;
                }
                if (attempted)
                {
                    if (clip == null && !string.IsNullOrEmpty(clipPath))
                    {
                        // The reimport replaced the native clip object.
                        foreach (UnityEngine.Object o in UnityEditor
                                     .AssetDatabase.LoadAllAssetsAtPath(clipPath))
                        {
                            if (o is AnimationClip c && c.name == clipName)
                            {
                                clip = c;
                                break;
                            }
                        }
                    }
                    if (clip != null)
                    {
                        animator.Rebind();
                        SampleTracks(clip, root, ordered, restRef, animator,
                                     viaGraph, sampleCount, tracks, motionNode,
                                     nodePos, nodeRot, nodeScale);
                        frozen = CountRotationTracksAtBind(tracks, bindRot,
                                                           sampleCount) * 100 >=
                                 ordered.Length * FreezeThresholdPercent;
                        healed = !frozen;
                    }
                    if (!healed)
                    {
                        foreach (string p in paths) s_healFailed.Add(p);
                    }
                }
            }
            if (animator != null)
            {
                animator.cullingMode = wasCulling;
            }

            StripRootMotion(clip, root, animator, motionNode, ordered, restRef,
                            tracks, bindPos, nodePos, nodeRot, nodeScale,
                            sampleCount, warnings);

            // A clip whose every sampled track is a constant produced a
            // character frozen in one pose. That is legitimate for a
            // deliberate pose clip and a defect for everything else, and
            // the difference is invisible in the container -- so say it
            // here, where the clip still has a name.
            if (warnings != null && clip.length > 0.05f)
            {
                bool moved = false;
                for (int i = 0; i < ordered.Length && !moved; i++)
                {
                    for (int s = 1; s < sampleCount && !moved; s++)
                    {
                        moved =
                            (tracks[i].Position[s] - tracks[i].Position[0])
                                .sqrMagnitude > 1e-10f ||
                            Quaternion.Dot(tracks[i].Rotation[s],
                                           tracks[i].Rotation[0]) < 0.999999f;
                    }
                }
                if (!moved)
                {
                    warnings.Add(
                        $"clip '{clip.name}' sampled as a constant pose -- no " +
                        "bone moves anywhere in it. If it animates in Play " +
                        "Mode, the export sampler could not drive this rig " +
                        "(check the Animator's avatar and culling mode).");
                }
                else if (healed)
                {
                    warnings.Add(
                        $"clip '{clip.name}': the Editor session had gone " +
                        "stale and sampled most bones at the BIND pose; the " +
                        "exporter force-reimported the character and " +
                        "recovered the motion. Nothing to do.");
                }
                else if (frozen)
                {
                    warnings.Add(
                        $"clip '{clip.name}': " +
                        $"{CountRotationTracksAtBind(tracks, bindRot, sampleCount)} " +
                        $"of {ordered.Length} bones never leave the BIND pose " +
                        "over the whole clip, even after a forced reimport of " +
                        "the character -- it will play half frozen (arms " +
                        "out). This is Editor-session state, not an asset " +
                        "problem: restart the Unity Editor and rebuild.");
                }
            }

            // Build the track table and key stream.
            var trackRecords = new List<byte[]>();
            var keyStream = new ByteBuffer();
            int keyCursor = 0;

            for (int i = 0; i < ordered.Length; i++)
            {
                // Translation.
                List<int> keep = ReducePosition(tracks[i].Position, positionTolerance);
                if (IsAnimated(tracks[i].Position, positionTolerance) || keep.Count > 1)
                {
                    float scaleMax = 0.0001f;
                    foreach (Vector3 v in tracks[i].Position)
                    {
                        scaleMax = Mathf.Max(scaleMax,
                            Mathf.Max(Mathf.Abs(v.x), Mathf.Max(Mathf.Abs(v.y), Mathf.Abs(v.z))));
                    }
                    trackRecords.Add(TrackRecord(i, 0, keep.Count, keyCursor, scaleMax));
                    foreach (int k in keep)
                    {
                        Vector3 v = tracks[i].Position[k];
                        WriteKey(keyStream, (float)k / (sampleCount - 1),
                                 v.x, v.y, v.z, 0.0f, scaleMax);
                    }
                    keyCursor += keep.Count;
                }

                // Rotation.
                List<int> keepRot = ReduceRotation(tracks[i].Rotation, rotationDotTolerance);
                trackRecords.Add(TrackRecord(i, 1, keepRot.Count, keyCursor, 1.0f));
                foreach (int k in keepRot)
                {
                    Quaternion q = tracks[i].Rotation[k];
                    WriteKey(keyStream, (float)k / (sampleCount - 1),
                             q.x, q.y, q.z, q.w, 1.0f);
                }
                keyCursor += keepRot.Count;

                // Scale, only when it actually moves.
                if (IsAnimated(tracks[i].Scale, scaleTolerance))
                {
                    List<int> keepScale = ReducePosition(tracks[i].Scale, scaleTolerance);
                    float scaleMax = 0.0001f;
                    foreach (Vector3 v in tracks[i].Scale)
                    {
                        scaleMax = Mathf.Max(scaleMax,
                            Mathf.Max(Mathf.Abs(v.x), Mathf.Max(Mathf.Abs(v.y), Mathf.Abs(v.z))));
                    }
                    trackRecords.Add(TrackRecord(i, 2, keepScale.Count, keyCursor, scaleMax));
                    foreach (int k in keepScale)
                    {
                        Vector3 v = tracks[i].Scale[k];
                        WriteKey(keyStream, (float)k / (sampleCount - 1),
                                 v.x, v.y, v.z, 0.0f, scaleMax);
                    }
                    keyCursor += keepScale.Count;
                }
            }

            var b = new ByteBuffer();
            b.U32((uint)P2bWriter.Fnv1a64(clip.name));
            b.F32(clip.length);
            b.U32((uint)trackRecords.Count);
            b.U32(loop ? 1u : 0u);
            b.U32((uint)keyCursor);
            b.U32(0);
            b.U32(0);
            b.U32(0);
            foreach (byte[] record in trackRecords)
            {
                b.Bytes(record);
            }
            b.Bytes(keyStream.ToArray());
            return b.ToArray();
        }

        // One full sampling pass over the fixed grid. The graph and playable
        // live entirely inside it, so a caller that has just repaired the
        // Editor session (reimport, rebind) gets a genuinely fresh pass, not
        // a re-evaluation through stale playable state.
        // Unity does not play a clip's root motion through the pose. For a
        // Generic rig the avatar's Root node (or the importer's Motion node)
        // carries the character's travel; the Animator EXTRACTS the channels
        // the clip's import settings leave unbaked -- Root Transform
        // Position (XZ) and (Y), Root Transform Rotation -- and either moves
        // the transform by them (Apply Root Motion) or drops them. Either
        // way the bone is pinned. This sampler reads the bone raw, so a walk
        // whose root travels forward walked ahead of its own capsule and
        // snapped back at the loop (the demo capybara). Strip what Unity
        // strips: the unbaked channels of the motion node, pinned at the
        // clip's reference ("Original" = the first sample; otherwise the
        // clip's average, standing in for Unity's centre of mass).
        //
        // Humanoid rigs derive root motion from the body's centre of mass
        // rather than a node; they are sampled through the Animator, which
        // already applies these settings, so nothing is done for them.
        private static void StripRootMotion(AnimationClip clip, GameObject root,
                                            Animator animator,
                                            Transform motionNode,
                                            Transform[] ordered,
                                            Transform[] restRef,
                                            SampledTrack[] tracks,
                                            Vector3[] bindPos,
                                            Vector3[] nodePos,
                                            Quaternion[] nodeRot,
                                            Vector3[] nodeScale,
                                            int sampleCount,
                                            List<string> warnings)
        {
            if (motionNode == null || sampleCount < 2)
            {
                return;
            }
            UnityEditor.AnimationClipSettings s =
                UnityEditor.AnimationUtility.GetAnimationClipSettings(clip);
            bool stripXZ = !s.loopBlendPositionXZ;
            bool stripY = !s.loopBlendPositionY;
            bool stripYaw = !s.loopBlendOrientation;
            if (!stripXZ && !stripY && !stripYaw)
            {
                return; // everything baked into the pose: Unity plays it too
            }

            // The node's reference: "Original" pins at the first sample,
            // otherwise at the clip's average (for Unity's centre of mass).
            Vector3 mean = Vector3.zero;
            for (int k = 0; k < sampleCount; k++) mean += nodePos[k];
            mean /= sampleCount;
            float refX = s.keepOriginalPositionXZ ? nodePos[0].x : mean.x;
            float refZ = s.keepOriginalPositionXZ ? nodePos[0].z : mean.z;
            float refY = s.keepOriginalPositionY ? nodePos[0].y : mean.y;
            float refYaw = 0f;
            if (stripYaw)
            {
                if (s.keepOriginalOrientation)
                {
                    refYaw = Yaw(nodeRot[0]);
                }
                else
                {
                    float sx = 0f, sy = 0f;
                    for (int k = 0; k < sampleCount; k++)
                    {
                        float y = Yaw(nodeRot[k]) * Mathf.Deg2Rad;
                        sx += Mathf.Cos(y);
                        sy += Mathf.Sin(y);
                    }
                    refYaw = Mathf.Atan2(sy, sx) * Mathf.Rad2Deg;
                }
            }

            // Per sample: the node as sampled, and the node with the
            // unbaked channels pinned. Every top-level track below the node
            // (its runtime parent is the Animator) is re-expressed as
            // pinned x inverse(sampled) x track, which leaves the bone's
            // own motion relative to the node untouched and removes only
            // what Unity would have extracted.
            var affected = new List<int>();
            for (int i = 0; i < ordered.Length; i++)
            {
                bool topLevel = restRef == null || restRef[i] == null ||
                                restRef[i] == root.transform;
                if (topLevel && ordered[i] != null &&
                    (ordered[i] == motionNode ||
                     ordered[i].IsChildOf(motionNode)))
                {
                    affected.Add(i);
                }
            }
            if (affected.Count == 0)
            {
                return;
            }

            float travelled = 0f;
            for (int k = 0; k < sampleCount; k++)
            {
                Vector3 p = nodePos[k];
                Quaternion r = nodeRot[k];
                Matrix4x4 sampled = Matrix4x4.TRS(p, r, nodeScale[k]);
                if (stripXZ)
                {
                    travelled = Mathf.Max(travelled,
                        Mathf.Abs(p.x - refX) + Mathf.Abs(p.z - refZ));
                    p.x = refX;
                    p.z = refZ;
                }
                if (stripY) p.y = refY;
                if (stripYaw)
                {
                    // Only the yaw delta about the Animator's up goes; the
                    // node's pitch and roll animation survives.
                    float delta = Mathf.DeltaAngle(refYaw, Yaw(r));
                    r = Quaternion.AngleAxis(-delta, Vector3.up) * r;
                }
                Matrix4x4 fix = Matrix4x4.TRS(p, r, nodeScale[k]) *
                                sampled.inverse;
                foreach (int i in affected)
                {
                    Matrix4x4 rel = Matrix4x4.TRS(tracks[i].Position[k],
                                                  tracks[i].Rotation[k],
                                                  tracks[i].Scale[k]);
                    Decompose(fix * rel, out tracks[i].Position[k],
                              out tracks[i].Rotation[k],
                              out tracks[i].Scale[k]);
                }
            }

            // Unity's "Center of Mass" reference keeps the body centred on
            // the transform rather than wherever the take happened to be
            // authored (a turn clip cut from frame 60 of a walking take
            // sits 2.3 units down the track). Pinning at the clip average
            // above still leaves the body at that offset; shift the
            // affected bones so their average over the clip is their
            // bind-pose position, the place the scene shows the character.
            bool centreXZ = stripXZ && !s.keepOriginalPositionXZ;
            bool centreY = stripY && !s.keepOriginalPositionY;
            if (centreXZ || centreY)
            {
                Vector3 shift = Vector3.zero;
                foreach (int i in affected)
                {
                    Vector3 avg = Vector3.zero;
                    for (int k = 0; k < sampleCount; k++) avg += tracks[i].Position[k];
                    avg /= sampleCount;
                    shift += bindPos[i] - avg;
                }
                shift /= affected.Count;
                if (!centreXZ) { shift.x = 0f; shift.z = 0f; }
                if (!centreY) shift.y = 0f;
                foreach (int i in affected)
                {
                    for (int k = 0; k < sampleCount; k++)
                    {
                        tracks[i].Position[k] += shift;
                    }
                }
            }

            if (warnings != null && animator != null &&
                animator.applyRootMotion && travelled > 0.01f)
            {
                warnings.Add(
                    $"clip '{clip.name}': Apply Root Motion is on, but the " +
                    "console never moves an entity by a clip's root motion; " +
                    "the character animates in place. Drive movement from a " +
                    "script (CharacterController.Move), as this demo does.");
            }
        }

        // The Generic rig's motion source, or null when Unity would use the
        // model root itself (which is the Animator: not a sampled bone, and
        // nothing below it inherits travel from it). Humanoid rigs derive
        // root motion from the body's centre of mass and are sampled
        // through the Animator, which applies the settings itself.
        private static Transform FindMotionNode(GameObject root,
                                                Animator animator,
                                                Transform[] ordered)
        {
            if (animator == null || animator.avatar == null || animator.isHuman)
            {
                return null;
            }
            string node = MotionNodeName(animator.avatar);
            if (string.IsNullOrEmpty(node))
            {
                return null;
            }
            Transform t = root.transform.Find(node);
            if (t == null)
            {
                int slash = node.LastIndexOf('/');
                string leaf = slash >= 0 ? node.Substring(slash + 1) : node;
                foreach (Transform any in root.GetComponentsInChildren<Transform>(true))
                {
                    if (any.name == leaf)
                    {
                        t = any;
                        break;
                    }
                }
            }
            return t == root.transform ? null : t;
        }

        // The importer's Motion node when set, else the avatar's Root node
        // (humanDescription's root motion bone, reachable only through
        // serialization).
        private static string MotionNodeName(Avatar avatar)
        {
            string path = UnityEditor.AssetDatabase.GetAssetPath(avatar);
            var importer = UnityEditor.AssetImporter.GetAtPath(path)
                               as UnityEditor.ModelImporter;
            if (importer == null)
            {
                return null;
            }
            if (!string.IsNullOrEmpty(importer.motionNodeName))
            {
                return importer.motionNodeName;
            }
            var so = new UnityEditor.SerializedObject(importer);
            UnityEditor.SerializedProperty p =
                so.FindProperty("m_HumanDescription.m_RootMotionBoneName");
            return p != null ? p.stringValue : null;
        }

        // Heading of a rotation about the parent's up axis, in degrees.
        private static float Yaw(Quaternion q)
        {
            Vector3 f = q * Vector3.forward;
            f.y = 0f;
            if (f.sqrMagnitude < 1e-8f)
            {
                Vector3 u = q * Vector3.up;
                f = new Vector3(u.x, 0f, u.z);
                if (f.sqrMagnitude < 1e-8f) return 0f;
            }
            return Mathf.Atan2(f.x, f.z) * Mathf.Rad2Deg;
        }

        private static void SampleTracks(AnimationClip clip, GameObject root,
                                         Transform[] ordered,
                                         Transform[] restRef,
                                         Animator animator, bool viaGraph,
                                         int sampleCount, SampledTrack[] tracks,
                                         Transform motionNode = null,
                                         Vector3[] nodePos = null,
                                         Quaternion[] nodeRot = null,
                                         Vector3[] nodeScale = null)
        {
            PlayableGraph graph = default;
            AnimationClipPlayable playable = default;
            bool wasLegacy = clip.legacy;
            if (viaGraph)
            {
                graph = PlayableGraph.Create("ps2-clip-bake");
                graph.SetTimeUpdateMode(DirectorUpdateMode.Manual);
                var output = AnimationPlayableOutput.Create(graph, "bake", animator);
                playable = AnimationClipPlayable.Create(graph, clip);
                // Raw motion: IK would re-solve feet against nothing and
                // bend what the clip actually says.
                playable.SetApplyFootIK(false);
                playable.SetApplyPlayableIK(false);
                output.SetSourcePlayable(playable);
            }
            else
            {
                clip.legacy = true; // SampleAnimation needs it outside play mode
            }
            for (int s = 0; s < sampleCount; s++)
            {
                float t = clip.length * s / (sampleCount - 1);
                if (viaGraph)
                {
                    playable.SetTime(t);
                    graph.Evaluate(0f);
                }
                else
                {
                    clip.SampleAnimation(root, t);
                }
                if (motionNode != null && nodePos != null)
                {
                    // Animator-relative, the space Unity extracts root
                    // motion in and the space of every top-level track.
                    Matrix4x4 nrel = root.transform.worldToLocalMatrix *
                                     motionNode.localToWorldMatrix;
                    Decompose(nrel, out nodePos[s], out nodeRot[s],
                              out nodeScale[s]);
                }
                for (int i = 0; i < ordered.Length; i++)
                {
                    // Keys in the SAME space as the rest pose: relative to
                    // the runtime parent (nearest bone ancestor, or the
                    // Animator for roots). A hips track sampled as a raw
                    // Unity local is relative to the ARMATURE node, and
                    // playback through a chain that folded the armature
                    // away would re-apply the wrong space every frame.
                    Transform refT = restRef != null ? restRef[i] : null;
                    if (refT != null)
                    {
                        Matrix4x4 rel = refT.worldToLocalMatrix *
                                        ordered[i].localToWorldMatrix;
                        Decompose(rel, out Vector3 kp, out Quaternion kr,
                                  out Vector3 ks);
                        tracks[i].Position[s] = kp;
                        tracks[i].Rotation[s] = kr;
                        tracks[i].Scale[s] = ks;
                    }
                    else
                    {
                        tracks[i].Position[s] = ordered[i].localPosition;
                        tracks[i].Rotation[s] = ordered[i].localRotation;
                        tracks[i].Scale[s] = ordered[i].localScale;
                    }
                }
            }
            if (viaGraph)
            {
                graph.Destroy();
            }
            else
            {
                clip.legacy = wasLegacy;
            }
        }

        // Rotation tracks whose whole sampled range stays at the bind
        // rotation -- the partial-freeze signature. A real humanoid clip
        // poses most mapped bones off bind; a stale session samples most of
        // them AT it (arms straight out, motion only in the stragglers).
        private static int CountRotationTracksAtBind(SampledTrack[] tracks,
                                                     Quaternion[] bindRot,
                                                     int sampleCount)
        {
            int atBind = 0;
            for (int i = 0; i < tracks.Length; i++)
            {
                bool pinned = true;
                for (int s = 0; s < sampleCount && pinned; s++)
                {
                    pinned = Mathf.Abs(Quaternion.Dot(tracks[i].Rotation[s],
                                                      bindRot[i])) > 0.995f;
                }
                if (pinned) atBind++;
            }
            return atBind;
        }

        // Asset paths whose forced reimport did NOT clear a partial freeze
        // this session: do not repeat a multi-second import per clip when it
        // has been shown not to help. An Editor restart -- the remaining
        // remedy -- clears statics too.
        private static readonly HashSet<string> s_healFailed =
            new HashSet<string>();

        // Percent of rotation tracks pinned at bind before a clip counts as
        // partially frozen. A field (not a literal) so a verification probe
        // can drop it to 0 and drive the reimport-and-resample path in a
        // session that is not actually stale.
        internal static int FreezeThresholdPercent = 90;

        private static byte[] TrackRecord(int bone, int channel, int keyCount,
                                          int keyFirst, float quantScale)
        {
            var t = new ByteBuffer();
            t.U16((ushort)bone);
            t.U8((byte)channel);
            t.U8(0);
            t.U16((ushort)keyCount);
            t.U16(0);
            t.U32((uint)keyFirst);
            t.F32(quantScale);
            return t.ToArray();
        }

        private static void WriteKey(ByteBuffer b, float normalizedTime, float x,
                                     float y, float z, float w, float quantScale)
        {
            ushort t = (ushort)Mathf.Clamp(Mathf.RoundToInt(normalizedTime * 65535.0f),
                                           0, 65535);
            b.U16(t);
            b.U16(0);
            float inv = quantScale != 0.0f ? 32767.0f / quantScale : 0.0f;
            WriteQuantised(b, x, inv);
            WriteQuantised(b, y, inv);
            WriteQuantised(b, z, inv);
            WriteQuantised(b, w, inv);
        }

        private static void WriteQuantised(ByteBuffer b, float value, float inv)
        {
            float scaled = Mathf.Clamp(value * inv, -32767.0f, 32767.0f);
            b.U16((ushort)(short)Mathf.RoundToInt(scaled));
        }

        private static bool IsAnimated(Vector3[] samples, float tolerance)
        {
            for (int i = 1; i < samples.Length; i++)
            {
                if ((samples[i] - samples[0]).magnitude > tolerance) return true;
            }
            return false;
        }

        // Greedy thinning: keep a sample only when dropping it would push the
        // linear reconstruction past the tolerance somewhere between the
        // surviving neighbours.
        private static List<int> ReducePosition(Vector3[] samples, float tolerance)
        {
            var keep = new List<int> { 0 };
            int last = 0;
            for (int candidate = 2; candidate < samples.Length; candidate++)
            {
                bool ok = true;
                for (int mid = last + 1; mid < candidate && ok; mid++)
                {
                    float u = (float)(mid - last) / (candidate - last);
                    Vector3 lerped = Vector3.Lerp(samples[last], samples[candidate], u);
                    ok = (lerped - samples[mid]).magnitude <= tolerance;
                }
                if (!ok)
                {
                    keep.Add(candidate - 1);
                    last = candidate - 1;
                }
            }
            keep.Add(samples.Length - 1);
            return keep;
        }

        private static List<int> ReduceRotation(Quaternion[] samples, float dotTolerance)
        {
            var keep = new List<int> { 0 };
            int last = 0;
            for (int candidate = 2; candidate < samples.Length; candidate++)
            {
                bool ok = true;
                for (int mid = last + 1; mid < candidate && ok; mid++)
                {
                    float u = (float)(mid - last) / (candidate - last);
                    Quaternion slerped =
                        Quaternion.Slerp(samples[last], samples[candidate], u);
                    ok = Mathf.Abs(Quaternion.Dot(slerped, samples[mid])) >= dotTolerance;
                }
                if (!ok)
                {
                    keep.Add(candidate - 1);
                    last = candidate - 1;
                }
            }
            keep.Add(samples.Length - 1);
            return keep;
        }

        // ---- controller ----------------------------------------------------

        public struct StateExport
        {
            public string Name;
            public int Clip;
            public float Speed;
            public bool Loop;
        }

        public struct TransitionExport
        {
            public int From;
            public int To;
            public float Duration;
            public byte Condition;
            public byte Param;
            public float Threshold;
        }

        // A 1D blend tree on a state (M12.5): the runtime blends the two
        // children whose thresholds bracket the parameter, phase-locked.
        public sealed class BlendTreeExport
        {
            public int State;
            public int Param;
            public List<(int clip, float threshold)> Children =
                new List<(int, float)>();
        }

        public static byte[] ExportController(StateExport[] states,
                                              TransitionExport[] transitions,
                                              string[] parameters,
                                              List<BlendTreeExport> trees = null)
        {
            var b = new ByteBuffer();
            b.U32((uint)states.Length);
            b.U32((uint)transitions.Length);
            b.U32((uint)parameters.Length);
            b.U32(0);
            foreach (StateExport s in states)
            {
                b.U32((uint)P2bWriter.Fnv1a64(s.Name));
                b.U16((ushort)s.Clip);
                b.U16(0);
                b.F32(s.Speed);
                b.U32(s.Loop ? 1u : 0u);
            }
            foreach (TransitionExport t in transitions)
            {
                b.U16((ushort)t.From);
                b.U16((ushort)t.To);
                b.F32(t.Duration);
                b.U8(t.Condition);
                b.U8(t.Param);
                b.U16(0);
                b.F32(t.Threshold);
            }
            foreach (string p in parameters)
            {
                b.U32((uint)P2bWriter.Fnv1a64(p));
            }
            // Blend-tree table, AFTER the params: readers that predate it
            // stop at the params, so the tail is compatible both ways. The
            // state's own clip field holds child 0, which is also what an
            // older runtime would play.
            b.U32((uint)(trees != null ? trees.Count : 0));
            if (trees != null)
            {
                foreach (BlendTreeExport t in trees)
                {
                    b.U16((ushort)t.State);
                    b.U8((byte)t.Param);
                    b.U8((byte)t.Children.Count);
                    foreach ((int clip, float threshold) in t.Children)
                    {
                        b.U16((ushort)clip);
                        b.U16(0);
                        b.F32(threshold);
                    }
                }
            }
            return b.ToArray();
        }

        // ---- skinned mesh --------------------------------------------------

        private sealed class Batch
        {
            public int FirstTriangle;
            public int TriangleCount;
            public List<int> Bones = new List<int>();
        }

        // 'textured' selects the 6-qword vertex layout (a texcoord qword after
        // the weights) and the ST+RGBAQ+XYZ2 batch tags for vu_skin_tex. The
        // header's flags bit0 records the choice for the loader.
        // 'boundsTransform' maps MESH space to the space the skinned verts
        // land in at runtime (the Animator's space): the culling sphere must
        // live where the character actually is, not at the raw mesh scale --
        // an FBX authored in centimetres has 0.01-unit mesh bounds under a
        // x100 node, and either mis-scale culls wrong.
        public static byte[] ExportSkinnedMesh(Mesh mesh, uint materialIndex,
                                               Color32 fallbackColour,
                                               Transform[] bones,
                                               Dictionary<Transform, int> boneIndex,
                                               Transform[] originalBones,
                                               int skeletonIndex,
                                               bool textured = false,
                                               Matrix4x4? boundsTransform = null,
                                               Matrix4x4? vertexTransform = null,
                                               int submesh = -1,
                                               bool preferVertexColours = true)
        {
            Vector3[] positions = mesh.vertices;
            Vector3[] normals = mesh.normals;
            if (vertexTransform.HasValue)
            {
                // Bake the renderer's node transform into the geometry so
                // every renderer of the character shares ONE mesh space (the
                // reference), which the union skeleton's single bind per
                // bone requires.
                Matrix4x4 vt = vertexTransform.Value;
                positions = (Vector3[])positions.Clone();
                for (int i = 0; i < positions.Length; i++)
                    positions[i] = vt.MultiplyPoint3x4(positions[i]);
                if (normals != null && normals.Length > 0)
                {
                    normals = (Vector3[])normals.Clone();
                    for (int i = 0; i < normals.Length; i++)
                        normals[i] = vt.MultiplyVector(normals[i]).normalized;
                }
            }
            Color32[] colours = mesh.colors32;
            Vector2[] uvs = textured ? mesh.uv : null;
            BoneWeight[] weights = mesh.boneWeights;
            // 'submesh' selects one material slot's triangles; -1 takes them
            // all (single-material meshes and the M9 callers). A renderer's
            // submeshes ARE its material slots, so exporting mesh.triangles
            // under material 0 painted a character's teeth and lashes in its
            // body colour.
            int[] indices = submesh >= 0 && submesh < mesh.subMeshCount
                                ? mesh.GetTriangles(submesh)
                                : mesh.triangles;
            int triangleCount = indices.Length / 3;
            // 6 qwords need 42 vertices to stay under the 8-bit VIF NUM
            // limit of 255 unpacked qwords; 5-qword batches keep their 48.
            int stride = textured ? 6 : 5;
            int maxVertsPerBatch = textured ? 42 : MaxSkinVertsPerBatch;

            // Unity's weight indices address the ORIGINAL bones[] order.
            var remap = new int[originalBones.Length];
            for (int i = 0; i < originalBones.Length; i++)
            {
                remap[i] = boneIndex[originalBones[i]];
            }

            // Per-triangle influence sets, in the exported bone order.
            var triangleBones = new List<int>[triangleCount];
            for (int t = 0; t < triangleCount; t++)
            {
                var set = new List<int>();
                for (int v = 0; v < 3; v++)
                {
                    foreach (int bone in InfluencesOf(weights[indices[t * 3 + v]], remap))
                    {
                        if (!set.Contains(bone)) set.Add(bone);
                    }
                }
                triangleBones[t] = set;
            }

            // Greedy partition (mirrors anim::partition_triangles).
            var batches = new List<Batch>();
            {
                Batch current = null;
                for (int t = 0; t < triangleCount; t++)
                {
                    if (triangleBones[t].Count > MaxPaletteBones)
                    {
                        throw new InvalidOperationException(
                            "[PS2] a triangle needs " + triangleBones[t].Count +
                            " bones; the palette holds " + MaxPaletteBones);
                    }
                    int additions = 0;
                    if (current != null)
                    {
                        foreach (int bone in triangleBones[t])
                        {
                            if (!current.Bones.Contains(bone)) additions++;
                        }
                    }
                    bool needNew = current == null ||
                                   current.TriangleCount * 3 + 3 > maxVertsPerBatch ||
                                   current.Bones.Count + additions > MaxPaletteBones;
                    if (needNew)
                    {
                        current = new Batch { FirstTriangle = t };
                        batches.Add(current);
                    }
                    foreach (int bone in triangleBones[t])
                    {
                        if (!current.Bones.Contains(bone)) current.Bones.Add(bone);
                    }
                    current.TriangleCount++;
                }
            }

            // When the whole skeleton fits one palette -- the common case for
            // a character rig -- give every batch the SAME full table so the
            // runtime uploads the palette once per character instead of once
            // per batch. That is worth ~90 redundant 114-qword DMA uploads
            // per character per frame.
            var union = new List<int>();
            foreach (Batch batch in batches)
            {
                foreach (int bone in batch.Bones)
                {
                    if (!union.Contains(bone)) union.Add(bone);
                }
            }
            if (union.Count <= MaxPaletteBones)
            {
                union.Sort();
                foreach (Batch batch in batches)
                {
                    batch.Bones = new List<int>(union);
                }
            }

            // Layout: 32-byte header, 16-byte descs, 64-byte bone tables,
            // pad to 16, then the vertex blobs.
            int descsBytes = batches.Count * 16;
            int tablesBytes = batches.Count * 64;
            int blobsStart = Align16(32 + descsBytes + tablesBytes);

            var header = new ByteBuffer();
            header.U32((uint)batches.Count);
            header.U32(materialIndex);
            Bounds bounds = mesh.bounds;
            Matrix4x4 bt = boundsTransform ?? Matrix4x4.identity;
            Vector3 bc = bt.MultiplyPoint3x4(bounds.center);
            Vector3 bs = bt.lossyScale;
            float bscale = Mathf.Max(Mathf.Abs(bs.x),
                                     Mathf.Max(Mathf.Abs(bs.y),
                                               Mathf.Abs(bs.z)));
            header.F32(bc.x);
            header.F32(bc.y);
            header.F32(bc.z);
            header.F32(bounds.extents.magnitude * bscale);
            header.U32((uint)skeletonIndex);
            header.U32(textured ? 1u : 0u); // flags: bit0 = 6-qword vertices

            var descs = new ByteBuffer();
            var tables = new ByteBuffer();
            var blobs = new ByteBuffer();
            int blobCursorQw = blobsStart / 16;

            foreach (Batch batch in batches)
            {
                int vertexCount = batch.TriangleCount * 3;
                int vertQw = vertexCount * stride;

                descs.U32((uint)blobCursorQw);
                descs.U32((uint)vertQw);
                descs.U32((uint)vertexCount);
                descs.U32(SkinVertDest);

                tables.U32((uint)batch.Bones.Count);
                for (int i = 0; i < MaxPaletteBones; i++)
                {
                    tables.U16(i < batch.Bones.Count ? (ushort)batch.Bones[i] : (ushort)0);
                }
                tables.U16(0); // pad to 64 bytes
                tables.U16(0);
                tables.U16(0);
                tables.U16(0);
                tables.U16(0);
                tables.U16(0);

                WriteGifTag(blobs, (uint)vertexCount, textured);
                blobs.U32((uint)vertexCount);
                blobs.U32(0);
                blobs.U64(0);

                for (int t = 0; t < batch.TriangleCount; t++)
                {
                    for (int v = 0; v < 3; v++)
                    {
                        int src = indices[(batch.FirstTriangle + t) * 3 + v];
                        Vector3 p = positions[src];
                        blobs.F32(p.x);
                        blobs.F32(p.y);
                        blobs.F32(p.z);
                        blobs.F32(1.0f);

                        Vector3 n = normals.Length > src ? normals[src] : Vector3.up;
                        blobs.F32(n.x);
                        blobs.F32(n.y);
                        blobs.F32(n.z);
                        blobs.F32(0.0f);

                        // Standard-family shaders ignore vertex colours, so
                        // a renderer WITH a material takes the material's
                        // colour (preferVertexColours false); the M9 rigs,
                        // which have no material, keep painting by vertex.
                        Color32 c = preferVertexColours && colours.Length > src
                                        ? colours[src] : fallbackColour;
                        // Textured characters MODULATE: the lit vertex colour
                        // multiplies the texel with 0x80 as 1.0, so it lives
                        // in 0..128 (same rule as the rigid tex layout;
                        // verify-log M12.5). Untextured characters keep
                        // 0..255 -- there the lit colour IS the final colour.
                        float cscale = textured ? 128.0f / 255.0f : 1.0f;
                        blobs.F32(c.r * cscale);
                        blobs.F32(c.g * cscale);
                        blobs.F32(c.b * cscale);
                        blobs.F32(128.0f); // PS2 alpha: 0x80 is opaque

                        // Palette offsets are INTEGERS the microprogram feeds
                        // straight to ILW: slot * 4 qwords per matrix.
                        BoneWeight bw = weights[src];
                        int[] slot = new int[4];
                        float[] weight = new float[4];
                        FillInfluences(bw, remap, batch.Bones, slot, weight);
                        for (int i = 0; i < 4; i++)
                        {
                            blobs.I32(slot[i] * 4);
                        }
                        for (int i = 0; i < 4; i++)
                        {
                            blobs.F32(weight[i]);
                        }

                        if (textured)
                        {
                            // (u, 1-v, 1, 0): v flips into GS raster
                            // orientation, exactly as the rigid textured
                            // path writes it; the 1 in z becomes Q after the
                            // VU's perspective divide.
                            Vector2 uv = uvs != null && uvs.Length > src
                                             ? uvs[src] : Vector2.zero;
                            blobs.F32(uv.x);
                            blobs.F32(1.0f - uv.y);
                            blobs.F32(1.0f);
                            blobs.F32(0.0f);
                        }
                    }
                }

                blobCursorQw += 2 + vertQw;
            }

            var section = new ByteBuffer();
            section.Bytes(header.ToArray());
            section.Bytes(descs.ToArray());
            section.Bytes(tables.ToArray());
            section.PadTo(16);
            section.Bytes(blobs.ToArray());
            return section.ToArray();
        }

        private static IEnumerable<int> InfluencesOf(BoneWeight bw, int[] remap)
        {
            if (bw.weight0 > 0.0f) yield return remap[bw.boneIndex0];
            if (bw.weight1 > 0.0f) yield return remap[bw.boneIndex1];
            if (bw.weight2 > 0.0f) yield return remap[bw.boneIndex2];
            if (bw.weight3 > 0.0f) yield return remap[bw.boneIndex3];
        }

        private static void FillInfluences(BoneWeight bw, int[] remap,
                                           List<int> table, int[] slot,
                                           float[] weight)
        {
            int[] bones = { remap[bw.boneIndex0], remap[bw.boneIndex1],
                            remap[bw.boneIndex2], remap[bw.boneIndex3] };
            float[] weights = { bw.weight0, bw.weight1, bw.weight2, bw.weight3 };
            float total = 0.0f;
            for (int i = 0; i < 4; i++)
            {
                if (weights[i] > 0.0f)
                {
                    int found = table.IndexOf(bones[i]);
                    if (found < 0)
                    {
                        throw new InvalidOperationException(
                            "[PS2] bone " + bones[i] + " missing from its batch table");
                    }
                    slot[i] = found;
                    weight[i] = weights[i];
                    total += weights[i];
                }
                else
                {
                    // Unused influences point at slot 0 with zero weight: the
                    // microprogram always reads four matrices, so the address
                    // must stay inside the palette.
                    slot[i] = 0;
                    weight[i] = 0.0f;
                }
            }
            // Renormalise: the microprogram's blended matrix assumes the
            // weights sum to one.
            if (total > 0.0f)
            {
                for (int i = 0; i < 4; i++) weight[i] /= total;
            }
            else
            {
                slot[0] = 0;
                weight[0] = 1.0f;
            }
        }

        private static void WriteGifTag(ByteBuffer b, uint nloop, bool textured)
        {
            // prim = triangle | IIP (gouraud) | TME when textured (FST stays
            // 0: STQ perspective-correct texturing, as the rigid path).
            ulong prim = 3UL | (1UL << 3);
            if (textured)
            {
                prim |= 1UL << 4;
            }
            ulong lo = (nloop & 0x7FFFUL)
                       | (1UL << 15)   // EOP
                       | (1UL << 46)   // PRE
                       | ((prim & 0x7FFUL) << 47)
                       | ((textured ? 3UL : 2UL) << 60); // NREG
            b.U64(lo);
            b.U64(textured ? 0x512UL   // ST, RGBAQ, XYZ2
                           : 0x51UL);  // RGBAQ, XYZ2
        }

        private static int Align16(int v) => (v + 15) & ~15;
    }
}
