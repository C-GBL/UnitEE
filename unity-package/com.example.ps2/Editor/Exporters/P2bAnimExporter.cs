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
                                        List<string> warnings = null)
        {
            int sampleCount = Mathf.Max(2, Mathf.RoundToInt(clip.length * sampleRate) + 1);
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
            PlayableGraph graph = default;
            AnimationClipPlayable playable = default;
            bool wasLegacy = clip.legacy;
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
            }
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
            if (animator != null)
            {
                animator.cullingMode = wasCulling;
            }

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
                        moved = (tracks[i].Position[s] - tracks[i].Position[0])
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

        public static byte[] ExportController(StateExport[] states,
                                              TransitionExport[] transitions,
                                              string[] parameters)
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
                                               Matrix4x4? vertexTransform = null)
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
            int[] indices = mesh.triangles;
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

                        Color32 c = colours.Length > src ? colours[src] : fallbackColour;
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
