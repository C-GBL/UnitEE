// M9 acceptance scene: three skinned characters (1,500 triangles, 24 bones
// each) plus an Editor-sampled golden pose trace (plan section 9, M9).
//
//   Unity -batchmode -quit -executeMethod Ps2.Editor.PS2SkinExportMenu.ExportSkinScene \
//         -ps2Output <scene.p2b> -ps2Golden <golden.bin>
//
// The characters are built procedurally so the scene is deterministic and
// needs no imported asset: a 24-bone chain wrapped in a tube mesh whose ring
// vertices are weighted across three neighbouring bones, which is what makes
// this a real skinning test rather than one-bone rigid attachment.
//
// The golden is sampled through Unity's OWN AnimationClip.SampleAnimation and
// its transform hierarchy, so the on-target comparison measures OUR
// quantisation, cursor sampling, slerp and bone composition against Unity --
// not against a second copy of our own maths.
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace Ps2.Editor
{
    internal static class PS2SkinExportMenu
    {
        private const int BoneCount = 24;
        private const int RingVerts = 32;
        private const float BoneSpacing = 0.34f;
        private const float TubeRadius = 0.26f;

        // Golden sampling: clip A across its whole 5 seconds, clip B at frame
        // resolution so a crossfade landing mid-clip can be checked exactly.
        private const int GoldenCountA = 30;
        private const int GoldenCountB = 60;
        private const float ClipALength = 5.0f;
        private const float ClipBLength = 2.0f;

        public static void ExportSkinScene()
        {
            string output = "skinscene.p2b";
            string golden = "skin-golden.bin";
            string[] args = System.Environment.GetCommandLineArgs();
            for (int i = 0; i < args.Length - 1; i++)
            {
                if (args[i] == "-ps2Output") output = args[i + 1];
                if (args[i] == "-ps2Golden") golden = args[i + 1];
            }

            Scene scene = SceneManager.GetActiveScene();
            foreach (GameObject root in scene.GetRootGameObjects())
            {
                Object.DestroyImmediate(root);
            }

            var camGo = new GameObject("Camera");
            Camera cam = camGo.AddComponent<Camera>();
            cam.fieldOfView = 55.0f;
            cam.nearClipPlane = 0.5f;
            cam.farClipPlane = 100.0f;
            cam.backgroundColor = new Color(20 / 255f, 24 / 255f, 40 / 255f, 1f);
            camGo.transform.position = new Vector3(0.0f, 4.0f, -11.0f);

            var lightGo = new GameObject("Sun");
            Light light = lightGo.AddComponent<Light>();
            light.type = LightType.Directional;
            light.color = Color.white;
            light.intensity = 0.9f;
            lightGo.transform.rotation = Quaternion.Euler(40.0f, -35.0f, 0.0f);

            // The reference character sits at the origin with an identity
            // transform, so a bone's localToWorldMatrix IS its model matrix --
            // exactly what the runtime's Animator::bone_world produces.
            GameObject reference = BuildCharacter("character", out Transform[] bones,
                                                  out SkinnedMeshRenderer smr);
            reference.transform.position = Vector3.zero;
            reference.transform.rotation = Quaternion.identity;

            AnimationClip clipA = BuildWaveClip(bones, ClipALength);
            AnimationClip clipB = BuildCoilClip(bones, ClipBLength);

            P2bAnimExporter.SkeletonExport skeleton =
                P2bAnimExporter.ExportSkeleton(smr.bones, smr.sharedMesh.bindposes);

            var payload = new P2bSceneExporter.SkinPayload
            {
                Skeleton = skeleton.Bytes,
                Controller = P2bAnimExporter.ExportController(
                    new[]
                    {
                        new P2bAnimExporter.StateExport
                        {
                            Name = "Wave", Clip = 0, Speed = 1.0f, Loop = false,
                        },
                        new P2bAnimExporter.StateExport
                        {
                            Name = "Coil", Clip = 1, Speed = 1.0f, Loop = true,
                        },
                    },
                    new[]
                    {
                        // Trigger-driven both ways: the sample decides when
                        // the crossfade happens, so the test is reproducible.
                        new P2bAnimExporter.TransitionExport
                        {
                            From = 0, To = 1, Duration = 0.5f, Condition = 5,
                            Param = 0, Threshold = 0.0f,
                        },
                        new P2bAnimExporter.TransitionExport
                        {
                            From = 1, To = 0, Duration = 0.5f, Condition = 5,
                            Param = 1, Threshold = 0.0f,
                        },
                    },
                    new[] { "ToCoil", "ToWave" }),
            };
            payload.Clips.Add(P2bAnimExporter.ExportClip(
                clipA, reference, skeleton.Ordered, skeleton.Index, 30.0f,
                /*loop=*/false, 0.0002f, 0.9999999f, 0.001f));
            payload.Clips.Add(P2bAnimExporter.ExportClip(
                clipB, reference, skeleton.Ordered, skeleton.Index, 30.0f,
                /*loop=*/true, 0.0002f, 0.9999999f, 0.001f));

            payload.SkinnedMeshes.Add(P2bAnimExporter.ExportSkinnedMesh(
                smr.sharedMesh, 0, new Color32(220, 190, 150, 255),
                skeleton.Ordered, skeleton.Index, smr.bones, 0));
            payload.RendererMesh[smr] = 0;
            // Group 0: the reference character, which check 1 compares against
            // the Unity-sampled golden through world.animator(0).
            payload.RendererGroup[smr] = 0;

            // The golden trace, sampled from Unity itself.
            WriteGolden(golden, reference, skeleton.Ordered, clipA, clipB);

            // Two more characters beside the reference. They share the mesh,
            // skeleton and controller; only their entity transforms differ.
            for (int i = 0; i < 2; i++)
            {
                GameObject extra = BuildCharacter("character" + (i + 1),
                                                  out Transform[] extraBones,
                                                  out SkinnedMeshRenderer extraSmr);
                extra.transform.position = new Vector3(i == 0 ? -3.6f : 3.6f, 0, 0);
                // Same mesh index -- three characters cost one SKMS -- but a
                // group of their own, because they are three CHARACTERS and
                // must animate independently. Check 2 drives world.animator(1)
                // through a crossfade while animator 0 holds the golden pose;
                // one shared animator would make that test meaningless.
                payload.RendererMesh[extraSmr] = 0;
                payload.RendererGroup[extraSmr] = i + 1;
            }

            // A material for the skinned kind (7): the classifier keys off
            // the shader name so the exporter records the right VU program.
            P2bSceneExporter.PendingSkin = payload;
            P2bSceneExporter.ExportActiveScene(output);
            P2bSceneExporter.PendingSkin = null;
            Debug.Log("[PS2] skin scene exported to " + output + " (golden " +
                      golden + ")");
        }

        // ---- character -----------------------------------------------------

        private static GameObject BuildCharacter(string name, out Transform[] bones,
                                                 out SkinnedMeshRenderer smr)
        {
            var root = new GameObject(name);
            bones = new Transform[BoneCount];
            Transform parent = root.transform;
            for (int i = 0; i < BoneCount; i++)
            {
                var boneGo = new GameObject("bone" + i);
                boneGo.transform.SetParent(parent, false);
                boneGo.transform.localPosition =
                    new Vector3(0.0f, i == 0 ? 0.0f : BoneSpacing, 0.0f);
                boneGo.transform.localRotation = Quaternion.identity;
                bones[i] = boneGo.transform;
                parent = boneGo.transform;
            }

            Mesh mesh = BuildTubeMesh(bones);
            smr = root.AddComponent<SkinnedMeshRenderer>();
            smr.bones = bones;
            smr.rootBone = bones[0];
            smr.sharedMesh = mesh;
            var material = new Material(Shader.Find("Unlit/Color"));
            material.color = new Color(0.86f, 0.75f, 0.6f, 1.0f);
            smr.sharedMaterial = material;
            return root;
        }

        // A tube around the bone chain: one ring of vertices per bone, each
        // ring vertex weighted across its own bone and both neighbours so the
        // surface bends smoothly (and so most triangles reference 4-6 bones,
        // which is what exercises the palette and the partitioner).
        private static Mesh BuildTubeMesh(Transform[] bones)
        {
            var vertices = new List<Vector3>();
            var normals = new List<Vector3>();
            var weights = new List<BoneWeight>();
            var triangles = new List<int>();

            for (int b = 0; b < BoneCount; b++)
            {
                float y = b * BoneSpacing;
                // Taper the ends so the silhouette is not a plain cylinder.
                float taper = Mathf.Sin(Mathf.PI * (b + 0.5f) / BoneCount);
                float radius = TubeRadius * (0.45f + 0.55f * taper);
                for (int r = 0; r < RingVerts; r++)
                {
                    float a = 2.0f * Mathf.PI * r / RingVerts;
                    var offset = new Vector3(Mathf.Cos(a) * radius, 0.0f,
                                             Mathf.Sin(a) * radius);
                    vertices.Add(new Vector3(offset.x, y, offset.z));
                    normals.Add(new Vector3(offset.x, 0.0f, offset.z).normalized);
                    weights.Add(RingWeight(b));
                }
            }

            for (int b = 0; b + 1 < BoneCount; b++)
            {
                int a0 = b * RingVerts;
                int b0 = (b + 1) * RingVerts;
                for (int r = 0; r < RingVerts; r++)
                {
                    int r1 = (r + 1) % RingVerts;
                    triangles.Add(a0 + r);
                    triangles.Add(b0 + r);
                    triangles.Add(a0 + r1);

                    triangles.Add(a0 + r1);
                    triangles.Add(b0 + r);
                    triangles.Add(b0 + r1);
                }
            }

            // Caps, as fans around a centre vertex on the first and last bone.
            int bottomCentre = vertices.Count;
            vertices.Add(new Vector3(0, 0, 0));
            normals.Add(Vector3.down);
            weights.Add(RingWeight(0));
            for (int r = 0; r < RingVerts; r++)
            {
                int r1 = (r + 1) % RingVerts;
                triangles.Add(bottomCentre);
                triangles.Add(r1);
                triangles.Add(r);
            }
            int topCentre = vertices.Count;
            vertices.Add(new Vector3(0, (BoneCount - 1) * BoneSpacing, 0));
            normals.Add(Vector3.up);
            weights.Add(RingWeight(BoneCount - 1));
            int lastRing = (BoneCount - 1) * RingVerts;
            for (int r = 0; r < RingVerts; r++)
            {
                int r1 = (r + 1) % RingVerts;
                triangles.Add(topCentre);
                triangles.Add(lastRing + r);
                triangles.Add(lastRing + r1);
            }

            var mesh = new Mesh();
            mesh.name = "character-tube";
            mesh.SetVertices(vertices);
            mesh.SetNormals(normals);
            mesh.SetTriangles(triangles, 0);
            mesh.boneWeights = weights.ToArray();

            var bindposes = new Matrix4x4[BoneCount];
            for (int i = 0; i < BoneCount; i++)
            {
                // Bind pose: mesh space -> bone space, with the character at
                // the identity (which BuildCharacter guarantees at export).
                bindposes[i] = bones[i].worldToLocalMatrix;
            }
            mesh.bindposes = bindposes;
            mesh.RecalculateBounds();
            return mesh;
        }

        private static BoneWeight RingWeight(int bone)
        {
            int lower = Mathf.Max(0, bone - 1);
            int upper = Mathf.Min(BoneCount - 1, bone + 1);
            var w = new BoneWeight
            {
                boneIndex0 = bone,
                weight0 = 0.5f,
                boneIndex1 = lower,
                weight1 = 0.25f,
                boneIndex2 = upper,
                weight2 = 0.25f,
                boneIndex3 = 0,
                weight3 = 0.0f,
            };
            return w;
        }

        // ---- clips ---------------------------------------------------------

        private static AnimationClip BuildWaveClip(Transform[] bones, float length)
        {
            var clip = new AnimationClip { name = "Wave", legacy = true };
            int frames = Mathf.RoundToInt(length * 30.0f);
            for (int b = 0; b < bones.Length; b++)
            {
                var cx = new AnimationCurve();
                var cy = new AnimationCurve();
                var cz = new AnimationCurve();
                var cw = new AnimationCurve();
                for (int f = 0; f <= frames; f++)
                {
                    float t = f / 30.0f;
                    float angle = 14.0f * Mathf.Sin(2.0f * Mathf.PI * t / length * 2.0f -
                                                    b * 0.42f);
                    Quaternion q = Quaternion.Euler(0.0f, 0.0f, angle);
                    cx.AddKey(t, q.x);
                    cy.AddKey(t, q.y);
                    cz.AddKey(t, q.z);
                    cw.AddKey(t, q.w);
                }
                string path = BonePath(bones, b);
                clip.SetCurve(path, typeof(Transform), "localRotation.x", cx);
                clip.SetCurve(path, typeof(Transform), "localRotation.y", cy);
                clip.SetCurve(path, typeof(Transform), "localRotation.z", cz);
                clip.SetCurve(path, typeof(Transform), "localRotation.w", cw);
            }
            return clip;
        }

        private static AnimationClip BuildCoilClip(Transform[] bones, float length)
        {
            var clip = new AnimationClip { name = "Coil", legacy = true };
            int frames = Mathf.RoundToInt(length * 30.0f);
            for (int b = 0; b < bones.Length; b++)
            {
                var cx = new AnimationCurve();
                var cy = new AnimationCurve();
                var cz = new AnimationCurve();
                var cw = new AnimationCurve();
                for (int f = 0; f <= frames; f++)
                {
                    float t = f / 30.0f;
                    float phase = 2.0f * Mathf.PI * t / length;
                    Quaternion q = Quaternion.Euler(
                        9.0f * Mathf.Sin(phase + b * 0.3f),
                        7.0f * Mathf.Cos(phase * 0.5f),
                        6.0f * Mathf.Sin(phase * 1.5f - b * 0.2f));
                    cx.AddKey(t, q.x);
                    cy.AddKey(t, q.y);
                    cz.AddKey(t, q.z);
                    cw.AddKey(t, q.w);
                }
                string path = BonePath(bones, b);
                clip.SetCurve(path, typeof(Transform), "localRotation.x", cx);
                clip.SetCurve(path, typeof(Transform), "localRotation.y", cy);
                clip.SetCurve(path, typeof(Transform), "localRotation.z", cz);
                clip.SetCurve(path, typeof(Transform), "localRotation.w", cw);
            }
            return clip;
        }

        private static string BonePath(Transform[] bones, int index)
        {
            string path = "bone0";
            for (int i = 1; i <= index; i++)
            {
                path += "/bone" + i;
            }
            return path;
        }

        // ---- golden --------------------------------------------------------

        // Layout (little-endian):
        //   u32 magic 'P2AG', u32 bone_count, u32 stride (16 floats)
        //   u32 countA, f32 stepA, u32 countB, f32 stepB
        //   f32 traceA[countA * bone_count * 16]
        //   f32 traceB[countB * bone_count * 16]
        // Matrices are model space, column-major, matching Unity's linear
        // Matrix4x4 indexing and the runtime's Mat4.
        private static void WriteGolden(string path, GameObject reference,
                                        Transform[] ordered, AnimationClip clipA,
                                        AnimationClip clipB)
        {
            float stepA = ClipALength / GoldenCountA;
            float stepB = 1.0f / 30.0f;

            var b = new ByteBuffer();
            b.U32(0x47413250u); // 'P2AG'
            b.U32((uint)ordered.Length);
            b.U32(16);
            b.U32(GoldenCountA);
            b.F32(stepA);
            b.U32(GoldenCountB);
            b.F32(stepB);

            SampleTrace(b, reference, ordered, clipA, GoldenCountA, stepA);
            SampleTrace(b, reference, ordered, clipB, GoldenCountB, stepB);

            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path)));
            File.WriteAllBytes(path, b.ToArray());
            Debug.Log("[PS2] golden pose trace: " + ordered.Length + " bones, " +
                      GoldenCountA + "+" + GoldenCountB + " samples -> " + path);
        }

        private static void SampleTrace(ByteBuffer b, GameObject reference,
                                        Transform[] ordered, AnimationClip clip,
                                        int count, float step)
        {
            for (int s = 0; s < count; s++)
            {
                clip.SampleAnimation(reference, s * step);
                for (int i = 0; i < ordered.Length; i++)
                {
                    Matrix4x4 m = ordered[i].localToWorldMatrix;
                    for (int e = 0; e < 16; e++)
                    {
                        b.F32(m[e]);
                    }
                }
            }
        }
    }
}
