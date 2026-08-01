// Scene -> .p2b (plan section 9 M5 tasks 4/6; SCEN layout per 10.4).
//
// Walks the active scene depth-first so parents always precede children --
// the property the runtime's one-pass world-matrix update depends on and
// refuses to run without. Collects the closed set of meshes and textures,
// deduplicated, and emits one file.
using System.Collections.Generic;
using UnityEditor;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace Ps2.Editor
{
    internal static class P2bSceneExporter
    {
        private struct EntityRecord
        {
            public Transform Transform;
            public int Parent;
            public int Mesh;     // index into the mesh list, -1 if none
            public bool IsCamera;
            public Camera Camera;
            public bool IsLight;
            public Light Light;
        }

        private sealed class MeshKey
        {
            public Mesh Mesh;
            public uint Kind;
            public uint MaterialIndex;
            public Color32 Fallback;
        }

        public static void ExportActiveScene(string path)
        {
            var entities = new List<EntityRecord>();
            var meshes = new List<MeshKey>();
            var meshLookup = new Dictionary<string, int>();
            var textures = new List<Texture2D>();
            var textureLookup = new Dictionary<Texture2D, int>();
            var materials = new List<(uint kind, uint texture)>();
            var materialLookup = new Dictionary<string, int>();

            foreach (GameObject root in SceneManager.GetActiveScene()
                         .GetRootGameObjects())
            {
                Walk(root.transform, -1, entities, meshes, meshLookup, textures,
                     textureLookup, materials, materialLookup);
            }

            var writer = new P2bWriter();

            // MATL first (order is irrelevant to the reader; indices are
            // per-type). One record per collected material.
            var matl = new ByteBuffer();
            foreach (var m in materials)
            {
                matl.U32(m.kind);
                matl.U32(m.texture);
                matl.F32(1);
                matl.F32(1);
                matl.F32(1);
                matl.F32(1);
            }
            writer.AddSection(P2bWriter.SectionMaterial, matl.ToArray());

            foreach (var key in meshes)
            {
                writer.AddSection(P2bWriter.SectionMesh,
                                  P2bMeshExporter.Export(key.Mesh, key.Kind,
                                                         key.MaterialIndex,
                                                         key.Fallback),
                                  key.Mesh.name);
            }
            foreach (Texture2D t in textures)
            {
                writer.AddSection(P2bWriter.SectionTex,
                                  P2bTextureExporter.Export(t), t.name);
            }

            writer.AddSection(P2bWriter.SectionScene, BuildScene(entities));
            writer.Write(path);
            Debug.Log($"[PS2] exported '{path}': {entities.Count} entities, " +
                      $"{meshes.Count} meshes, {textures.Count} textures, " +
                      $"{materials.Count} materials");
        }

        private static void Walk(Transform t, int parent,
                                 List<EntityRecord> entities, List<MeshKey> meshes,
                                 Dictionary<string, int> meshLookup,
                                 List<Texture2D> textures,
                                 Dictionary<Texture2D, int> textureLookup,
                                 List<(uint, uint)> materials,
                                 Dictionary<string, int> materialLookup)
        {
            var record = new EntityRecord
            {
                Transform = t,
                Parent = parent,
                Mesh = -1,
            };

            var filter = t.GetComponent<MeshFilter>();
            var renderer = t.GetComponent<MeshRenderer>();
            if (filter != null && renderer != null && filter.sharedMesh != null)
            {
                Mesh mesh = filter.sharedMesh;
                Material mat = renderer.sharedMaterial;
                var mainTex = mat != null ? mat.mainTexture as Texture2D : null;

                uint kind;
                uint texIndex = 0xFFFFFFFF;
                if (mainTex != null)
                {
                    kind = P2bMeshExporter.KindUnlitTextured;
                    if (!textureLookup.TryGetValue(mainTex, out int ti))
                    {
                        ti = textures.Count;
                        textures.Add(mainTex);
                        textureLookup.Add(mainTex, ti);
                    }
                    texIndex = (uint)ti;
                }
                else if (mesh.normals != null && mesh.normals.Length > 0)
                {
                    kind = P2bMeshExporter.KindVertexLit;
                }
                else
                {
                    kind = P2bMeshExporter.KindUnlit;
                }

                string matKey = kind + ":" + texIndex;
                if (!materialLookup.TryGetValue(matKey, out int mi))
                {
                    mi = materials.Count;
                    materials.Add((kind, texIndex));
                    materialLookup.Add(matKey, mi);
                }

                Color32 fallback = mat != null ? (Color32)mat.color
                                               : new Color32(255, 255, 255, 255);
                string meshKey = mesh.GetInstanceID() + ":" + kind + ":" + mi +
                                 ":" + fallback.r + "," + fallback.g + "," +
                                 fallback.b;
                if (!meshLookup.TryGetValue(meshKey, out int meshIndex))
                {
                    meshIndex = meshes.Count;
                    meshes.Add(new MeshKey
                    {
                        Mesh = mesh,
                        Kind = kind,
                        MaterialIndex = (uint)mi,
                        Fallback = fallback,
                    });
                    meshLookup.Add(meshKey, meshIndex);
                }
                record.Mesh = meshIndex;
            }

            var camera = t.GetComponent<Camera>();
            if (camera != null)
            {
                record.IsCamera = true;
                record.Camera = camera;
            }
            var light = t.GetComponent<Light>();
            if (light != null && light.type == LightType.Directional)
            {
                record.IsLight = true;
                record.Light = light;
            }

            int myIndex = entities.Count;
            entities.Add(record);
            foreach (Transform child in t)
            {
                Walk(child, myIndex, entities, meshes, meshLookup, textures,
                     textureLookup, materials, materialLookup);
            }
        }

        private static byte[] BuildScene(List<EntityRecord> entities)
        {
            // Components are laid out entity-by-entity, so component_first is
            // sequential. Payloads follow the ref table.
            var comps = new List<(ushort type, byte[] payload)>();
            var perEntity = new List<(int first, int count)>();
            foreach (var e in entities)
            {
                int first = comps.Count;
                if (e.Mesh >= 0)
                {
                    var p = new ByteBuffer();
                    p.U32((uint)e.Mesh);
                    p.U32(0xFFFFFFFF); // no material override
                    comps.Add((1, p.ToArray()));
                }
                if (e.IsCamera)
                {
                    var p = new ByteBuffer();
                    p.F32(e.Camera.fieldOfView * Mathf.Deg2Rad);
                    p.F32(e.Camera.nearClipPlane);
                    p.F32(e.Camera.farClipPlane);
                    comps.Add((2, p.ToArray()));
                }
                if (e.IsLight)
                {
                    var p = new ByteBuffer();
                    Vector3 dir = e.Light.transform.forward;
                    p.F32(dir.x);
                    p.F32(dir.y);
                    p.F32(dir.z);
                    p.F32(e.Light.color.r * e.Light.intensity);
                    p.F32(e.Light.color.g * e.Light.intensity);
                    p.F32(e.Light.color.b * e.Light.intensity);
                    comps.Add((3, p.ToArray()));
                }
                perEntity.Add((first, comps.Count - first));
            }

            var b = new ByteBuffer();
            b.U32((uint)entities.Count);
            b.U32((uint)comps.Count);
            b.U32(0); // name table

            for (int i = 0; i < entities.Count; i++)
            {
                var e = entities[i];
                Transform t = e.Transform;
                b.I32(e.Parent);
                b.F32(t.localPosition.x);
                b.F32(t.localPosition.y);
                b.F32(t.localPosition.z);
                b.F32(t.localRotation.x);
                b.F32(t.localRotation.y);
                b.F32(t.localRotation.z);
                b.F32(t.localRotation.w);
                b.F32(t.localScale.x);
                b.F32(t.localScale.y);
                b.F32(t.localScale.z);
                b.U32((uint)P2bWriter.Fnv1a64(t.name));
                b.U16(0); // layer
                b.U16(0); // tag
                b.U32(0); // flags
                b.U32((uint)perEntity[i].first);
                b.U16((ushort)perEntity[i].count);
                b.U16(0); // pad
            }

            // Ref table, then payloads.
            int refsAt = (int)b.Position;
            int payloadAt = refsAt + comps.Count * 8;
            var offsets = new int[comps.Count];
            for (int i = 0; i < comps.Count; i++)
            {
                offsets[i] = payloadAt;
                payloadAt += comps[i].payload.Length;
            }
            for (int i = 0; i < comps.Count; i++)
            {
                b.U16(comps[i].type);
                b.U16(0);
                b.U32((uint)offsets[i]);
            }
            foreach (var c in comps)
            {
                b.Bytes(c.payload);
            }
            return b.ToArray();
        }
    }

    internal static class PS2ExportMenu
    {
        [MenuItem("PS2/Export Current Scene")]
        public static void ExportCurrentScene()
        {
            string path = EditorUtility.SaveFilePanel("Export PS2 scene", "",
                                                      "scene", "p2b");
            if (!string.IsNullOrEmpty(path))
            {
                P2bSceneExporter.ExportActiveScene(path);
            }
        }

        // Batch entry: builds the deterministic M5 acceptance scene (>=20
        // meshes, >=10 textures, camera, directional light) and exports it.
        //   Unity -batchmode -quit -executeMethod Ps2.Editor.PS2ExportMenu.ExportTestScene -ps2Output <path>
        public static void ExportTestScene()
        {
            string output = "m5-scene.p2b";
            string[] args = System.Environment.GetCommandLineArgs();
            for (int i = 0; i < args.Length - 1; i++)
            {
                if (args[i] == "-ps2Output")
                {
                    output = args[i + 1];
                }
            }

            BuildTestScene();
            P2bSceneExporter.ExportActiveScene(output);
            Debug.Log("[PS2] test scene exported to " + output);
        }

        private static void BuildTestScene()
        {
            var scene = SceneManager.GetActiveScene();
            foreach (GameObject root in scene.GetRootGameObjects())
            {
                Object.DestroyImmediate(root);
            }

            // Camera looking down +z from a step back.
            var camGo = new GameObject("Camera");
            var cam = camGo.AddComponent<Camera>();
            cam.fieldOfView = 60.0f;
            cam.nearClipPlane = 0.5f;
            cam.farClipPlane = 100.0f;
            camGo.transform.position = new Vector3(0, 2.0f, -10.0f);
            camGo.transform.rotation = Quaternion.Euler(8.0f, 0, 0);

            var lightGo = new GameObject("Sun");
            var light = lightGo.AddComponent<Light>();
            light.type = LightType.Directional;
            light.color = Color.white;
            light.intensity = 0.9f;
            lightGo.transform.rotation = Quaternion.Euler(50.0f, -30.0f, 0);

            // 10 procedural textures with distinct patterns.
            var texMaterials = new Material[10];
            for (int i = 0; i < 10; i++)
            {
                var tex = new Texture2D(64, 64, TextureFormat.RGBA32, false);
                var px = new Color32[64 * 64];
                for (int y = 0; y < 64; y++)
                {
                    for (int x = 0; x < 64; x++)
                    {
                        bool check = (((x >> 3) + (y >> 3)) & 1) == 0;
                        byte r = (byte)(check ? 40 + i * 20 : 220 - i * 15);
                        byte g = (byte)(check ? 220 - i * 18 : 60 + i * 12);
                        byte bch = (byte)(check ? 90 + i * 14 : 200 - i * 10);
                        px[y * 64 + x] = new Color32(r, g, bch, 255);
                    }
                }
                tex.SetPixels32(px);
                tex.Apply();
                tex.name = "ps2tex" + i;
                var mat = new Material(Shader.Find("Unlit/Texture"));
                mat.mainTexture = tex;
                texMaterials[i] = mat;
            }

            // 20 meshes: 10 textured cubes, 6 lit spheres, 4 flat-colour cubes
            // (colour baked as vertex colour by the exporter), some parented.
            GameObject firstCube = null;
            for (int i = 0; i < 10; i++)
            {
                var go = GameObject.CreatePrimitive(PrimitiveType.Cube);
                go.name = "texcube" + i;
                go.GetComponent<MeshRenderer>().sharedMaterial = texMaterials[i];
                go.transform.position =
                    new Vector3(-6.0f + (i % 5) * 3.0f, (i / 5) * 3.0f - 1.0f, 2.0f);
                go.transform.rotation = Quaternion.Euler(0, 20.0f * i, 0);
                if (i == 0)
                {
                    firstCube = go;
                }
            }
            for (int i = 0; i < 6; i++)
            {
                var go = GameObject.CreatePrimitive(PrimitiveType.Sphere);
                go.name = "litsphere" + i;
                var mat = new Material(Shader.Find("Unlit/Color"));
                mat.color = new Color(0.9f, 0.85f, 0.8f, 1.0f);
                mat.mainTexture = null;
                go.GetComponent<MeshRenderer>().sharedMaterial = mat;
                go.transform.position =
                    new Vector3(-5.0f + i * 2.0f, 4.5f, 3.0f);
            }
            for (int i = 0; i < 3; i++)
            {
                var go = GameObject.CreatePrimitive(PrimitiveType.Cube);
                go.name = "flatcube" + i;
                var mat = new Material(Shader.Find("Unlit/Color"));
                mat.color = new Color(0.2f + 0.3f * i, 0.5f, 0.9f - 0.3f * i, 1.0f);
                go.GetComponent<MeshRenderer>().sharedMaterial = mat;
                // Parent under the first textured cube: hierarchy exercise.
                go.transform.SetParent(firstCube.transform, false);
                go.transform.localPosition = new Vector3(0, 1.5f + i * 1.2f, 0);
                go.transform.localScale = Vector3.one * 0.5f;
            }
            {
                // The 20th mesh: a big floor quad from a cube.
                var go = GameObject.CreatePrimitive(PrimitiveType.Cube);
                go.name = "floor";
                var mat = new Material(Shader.Find("Unlit/Color"));
                mat.color = new Color(0.25f, 0.3f, 0.35f, 1.0f);
                go.GetComponent<MeshRenderer>().sharedMaterial = mat;
                go.transform.position = new Vector3(0, -2.5f, 3.0f);
                go.transform.localScale = new Vector3(18.0f, 0.3f, 12.0f);
            }
        }
    }
}
