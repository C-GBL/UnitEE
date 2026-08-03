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
            public List<string> Scripts; // managed type names, or null
            public SkinnedMeshRenderer Skinned; // M9, or null
            public Rigidbody Body;              // M11, or null
            public Animator Animator;           // M12.5, or null
        }

        // The rig sections. Normally baked from the scene's own assets by
        // P2bRigExporter (M12.5 task 1); a caller that owns its rig may set
        // this first and is then responsible for clearing it, which is how
        // the M9 acceptance scene supplies a procedurally built character.
        internal sealed class SkinPayload
        {
            public byte[] Skeleton;
            public List<byte[]> Clips = new List<byte[]>();
            public byte[] Controller;
            // One SKMS section per exported renderer. A character imported
            // from a DCC tool is many renderers over one skeleton -- body,
            // hair, each cloth piece -- because that is how its materials are
            // assigned, so this is a list, not a single mesh.
            public List<byte[]> SkinnedMeshes = new List<byte[]>();
            // The texture each SkinnedMeshes entry samples, or null for the
            // vertex-coloured 5-qword format. The scene exporter turns these
            // into TEX sections and MATL records and patches each mesh's
            // material_index -- the rig baker cannot, because texture and
            // material indices are scene-wide.
            public List<Texture2D> MeshTextures = new List<Texture2D>();
            // Which SkinnedMeshes index each renderer draws. Several renderers
            // may share one entry: a crowd of the same character costs one
            // mesh and one controller.
            public Dictionary<SkinnedMeshRenderer, int> RendererMesh =
                new Dictionary<SkinnedMeshRenderer, int>();
            // Which CHARACTER each renderer belongs to. Renderers in one group
            // share an animator; separate groups animate independently. The
            // runtime cannot work this out for itself -- one character's many
            // renderers and many characters of one rig look identical from
            // the skeleton and controller alone.
            public Dictionary<SkinnedMeshRenderer, int> RendererGroup =
                new Dictionary<SkinnedMeshRenderer, int>();
        }

        internal static SkinPayload PendingSkin;

        // The profile's Texture Max Size, set by the build step before it
        // exports. 256 is the default a profile ships with, and it is what a
        // scene exported from a menu item (no profile in sight) gets: at
        // 512x448 the framebuffers leave about 1.4 MB of VRAM, so one
        // 1024x1024 PSMT8 texture would not fit on its own.
        internal static int MaxTextureSize = 256;

        // Pre-built SND section (M10), supplied by the audio export
        // entry point the same way PendingSkin supplies the rig.
        internal static byte[] PendingSound;

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

            GameObject[] roots = SceneManager.GetActiveScene().GetRootGameObjects();

            // Rigs, BEFORE the walk: a skinned renderer names material 0, so
            // the material table has to know whether there is one.
            //
            // PendingSkin used to be the only way in, set by a bespoke export
            // menu that built a procedural test rig. A caller that has already
            // baked a rig still wins (the M9 acceptance scene does exactly
            // that); anything else gets its rig read from the scene's own
            // assets, which is what makes an imported character work.
            var rigWarnings = new List<string>();
            bool autoBakedRig = false;
            if (PendingSkin == null)
            {
                PendingSkin = P2bRigExporter.Bake(roots, rigWarnings);
                autoBakedRig = PendingSkin != null;
                foreach (string warning in rigWarnings)
                    Debug.LogWarning("[PS2] " + warning);
            }

            // Skinned materials, BEFORE the walk so their indices are known
            // (walk materials append after). One record per distinct texture
            // -- kind Skinned throughout; the texture index is what differs.
            // The old form of this registered one untextured material and,
            // worse, ran before the auto-bake existed, so an auto-baked rig
            // got no skinned material at all and its material_index 0 aliased
            // whatever the walk found first. Harmless while the skinned pass
            // ignored materials; fatal the moment it binds textures by them.
            var skinMaterialOf = new List<int>();
            if (PendingSkin != null)
            {
                for (int i = 0; i < PendingSkin.SkinnedMeshes.Count; i++)
                {
                    Texture2D tex = i < PendingSkin.MeshTextures.Count
                                        ? PendingSkin.MeshTextures[i] : null;
                    uint texIndex = 0xFFFFFFFF;
                    if (tex != null)
                    {
                        if (!textureLookup.TryGetValue(tex, out int ti))
                        {
                            ti = textures.Count;
                            textures.Add(tex);
                            textureLookup[tex] = ti;
                        }
                        texIndex = (uint)ti;
                    }
                    string key = P2bMeshExporter.KindSkinned + ":" + texIndex;
                    if (!materialLookup.TryGetValue(key, out int mi))
                    {
                        mi = materials.Count;
                        materials.Add((P2bMeshExporter.KindSkinned, texIndex));
                        materialLookup[key] = mi;
                    }
                    skinMaterialOf.Add(mi);
                }
            }

            foreach (GameObject root in roots)
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
                // MATL v2 (M8 task 5): GS TEST/ALPHA precomputed as data.
                matl.U64(GsTestFor(m.kind));
                matl.U64(GsAlphaFor(m.kind));
                matl.U32(MaterialFlags(m.kind));
                matl.U32(0);
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
                                  P2bTextureExporter.Export(t, MaxTextureSize),
                                  t.name);
            }

            // SCRP: deduplicated NUL-terminated script type names; SCEN
            // script components store offsets into it.
            var scriptNames = new Dictionary<string, uint>();
            var scrp = new ByteBuffer();
            int scriptComponents = 0;
            foreach (var e in entities)
            {
                if (e.Scripts == null) continue;
                foreach (string s in e.Scripts)
                {
                    scriptComponents++;
                    if (scriptNames.ContainsKey(s)) continue;
                    scriptNames[s] = (uint)scrp.Position;
                    foreach (char c in s) scrp.U8((byte)c); // ASCII by contract
                    scrp.U8(0);
                }
            }
            if (scrp.Position > 0)
            {
                writer.AddSection(P2bWriter.SectionScripts, scrp.ToArray());
            }

            // M9 sections, in the order the loader wants them available:
            // skeletons before clips before controllers before skinned
            // meshes (each validates against the previous).
            if (PendingSkin != null)
            {
                writer.AddSection(P2bWriter.SectionSkeleton, PendingSkin.Skeleton);
                foreach (byte[] clip in PendingSkin.Clips)
                {
                    writer.AddSection(P2bWriter.SectionClip, clip);
                }
                writer.AddSection(P2bWriter.SectionController, PendingSkin.Controller);
                for (int i = 0; i < PendingSkin.SkinnedMeshes.Count; i++)
                {
                    // The rig baker wrote material_index 0 as a placeholder --
                    // it cannot know scene-wide indices. Patch the header's
                    // u32 at offset 4 with the material registered above.
                    byte[] skinned = PendingSkin.SkinnedMeshes[i];
                    uint mat = i < skinMaterialOf.Count
                                   ? (uint)skinMaterialOf[i] : 0u;
                    skinned[4] = (byte)mat;
                    skinned[5] = (byte)(mat >> 8);
                    skinned[6] = (byte)(mat >> 16);
                    skinned[7] = (byte)(mat >> 24);
                    writer.AddSection(P2bWriter.SectionSkinnedMesh, skinned);
                }
            }

            if (PendingSound != null)
            {
                writer.AddSection(P2bWriter.SectionSound, PendingSound);
            }

            // PHYS (M11 task 1). Colliders are baked against the entity table
            // built by the walk above, so this has to run after it: a collider
            // record names the entity index it rides on, and a primitive
            // collider whose entity resolved to -1 would be static geometry
            // that can never move.
            //
            // This is deliberately unconditional. Baking is cheap when a scene
            // has no colliders (an empty BVH is one degenerate node), and the
            // alternative -- exporting collision only when someone remembers
            // to ask -- is exactly how the section came to be missing from
            // every build until now.
            var entityOf = new Dictionary<GameObject, int>();
            for (int i = 0; i < entities.Count; i++)
            {
                entityOf[entities[i].Transform.gameObject] = i;
            }
            P2bPhysicsExporter.BakeResult phys = P2bPhysicsExporter.Bake(
                roots, go => entityOf.TryGetValue(go, out int index) ? index : -1);
            foreach (string warning in phys.Warnings)
            {
                Debug.LogWarning("[PS2] " + warning);
            }
            // A null payload means the bake refused (the vertex cap); the
            // warning above says why. Writing a section anyway would ship
            // collision that silently disagrees with the scene.
            if (phys.Payload != null)
            {
                writer.AddSection(P2bWriter.SectionPhysics, phys.Payload);
            }

            writer.AddSection(P2bWriter.SectionScene, BuildScene(entities, scriptNames));
            writer.Write(path);

            // A rig this call baked belongs to THIS scene. PendingSkin is
            // static, so leaving it set would carry scene 1's character into
            // scene 2 of a multi-scene build -- the same shape as the M10 bug
            // where World::load did not reset its animation counters and every
            // third load overflowed them. A rig a CALLER supplied is theirs to
            // clear, and PS2SkinExportMenu does.
            if (autoBakedRig)
                PendingSkin = null;
            int bodies = 0;
            foreach (var e in entities)
            {
                if (e.Body != null) bodies++;
            }
            Debug.Log($"[PS2] exported '{path}': {entities.Count} entities, " +
                      $"{meshes.Count} meshes, {textures.Count} textures, " +
                      $"{materials.Count} materials, {scriptComponents} scripts, " +
                      $"{phys.ColliderCount} colliders, {bodies} rigidbodies, " +
                      $"{phys.TriangleCount} collision triangles");
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

                uint kind = ClassifyMaterial(mat, mainTex != null,
                                             mesh.normals != null &&
                                             mesh.normals.Length > 0);
                uint texIndex = 0xFFFFFFFF;
                if (kind == P2bMeshExporter.KindUnlitTextured)
                {
                    if (!textureLookup.TryGetValue(mainTex, out int ti))
                    {
                        ti = textures.Count;
                        textures.Add(mainTex);
                        textureLookup.Add(mainTex, ti);
                    }
                    texIndex = (uint)ti;
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
                                 fallback.b + "," + fallback.a;
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

            var skinned = t.GetComponent<SkinnedMeshRenderer>();
            if (skinned != null && PendingSkin != null &&
                PendingSkin.RendererMesh.ContainsKey(skinned))
            {
                record.Skinned = skinned;
            }

            // Rigidbody (M11). The COLLIDER travels in the PHYS section and the
            // BODY travels here, because they are different kinds of thing: a
            // collider is baked geometry the solver reads, a body is component
            // state the managed Rigidbody owns. Keeping the body in SCEN is
            // what lets GetComponent<Rigidbody>() find one.
            record.Body = t.GetComponent<Rigidbody>();

            // The Animator rides on the entity that HAS it, which is not
            // always the one carrying the SkinnedMeshRenderer -- Unity's own
            // import puts the Animator on the model root and the renderer on
            // a child. Finding it through the renderer, as the runtime did
            // until now, is the same indirection that made Rigidbody
            // unreachable from GetComponent.
            record.Animator = t.GetComponent<Animator>();

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

            // User scripts (M7): every MonoBehaviour from the user's script
            // assemblies rides along as a type reference. The PS2 build
            // recompiles those sources against the shim into an assembly of
            // the SAME name, so "Full.Name, Assembly-CSharp" resolves via
            // Type.GetType on target. Package/editor scripts never export.
            foreach (var mb in t.GetComponents<MonoBehaviour>())
            {
                if (mb == null) continue; // missing-script placeholder
                var type = mb.GetType();
                string assembly = type.Assembly.GetName().Name;
                if (!assembly.StartsWith("Assembly-CSharp")) continue;
                record.Scripts ??= new List<string>();
                record.Scripts.Add(type.FullName + ", " + assembly);
            }

            int myIndex = entities.Count;
            entities.Add(record);
            foreach (Transform child in t)
            {
                Walk(child, myIndex, entities, meshes, meshLookup, textures,
                     textureLookup, materials, materialLookup);
            }
        }

        // Kind selection (plan 7.3): explicit transparent/cutout/additive
        // classification from the material, then the M5 texture/normals
        // fallback. Standard "Transparent" rendering mode and anything at or
        // past the transparent queue maps to LitAlpha.
        private static uint ClassifyMaterial(Material mat, bool hasTexture,
                                             bool hasNormals)
        {
            if (mat != null)
            {
                string shaderName = mat.shader != null ? mat.shader.name : "";
                string renderType = mat.GetTag("RenderType", false, "");
                if (shaderName.Contains("Additive"))
                    return P2bMeshExporter.KindAdditive;
                if (renderType == "TransparentCutout")
                    return P2bMeshExporter.KindCutout;
                if (renderType == "Transparent" || mat.renderQueue >= 3000)
                    return P2bMeshExporter.KindLitAlpha;
            }
            if (hasTexture) return P2bMeshExporter.KindUnlitTextured;
            if (hasNormals)
                return RenderSettings.fog ? P2bMeshExporter.KindVertexLitFog
                                          : P2bMeshExporter.KindVertexLit;
            return P2bMeshExporter.KindUnlit;
        }

        // GS TEST register value, or 0 for "device default" (depth GEQUAL,
        // no alpha test). Bit layout matches ps2ur::gfx::gs_test.
        private static ulong GsTestFor(uint kind)
        {
            if (kind == P2bMeshExporter.KindCutout)
            {
                // ATE on, ATST=GEQUAL(5), AREF=64 (0.5 in PS2 alpha), AFAIL=
                // KEEP(0), depth test GEQUAL(2) on.
                return 1UL | (5UL << 1) | (64UL << 4) | (1UL << 16) | (2UL << 17);
            }
            return 0;
        }

        // GS ALPHA register: Cv = ((A-B)*C >> 7) + D.
        private static ulong GsAlphaFor(uint kind)
        {
            if (kind == P2bMeshExporter.KindLitAlpha)
                return 0UL | (1UL << 2) | (0UL << 4) | (1UL << 6); // (Cs-Cd)*As+Cd
            if (kind == P2bMeshExporter.KindAdditive)
                return 0UL | (2UL << 2) | (0UL << 4) | (1UL << 6); // Cs*As+Cd
            return 0;
        }

        // bit0 zwrite, bit1 blend, bit2 transparent-pass.
        private static uint MaterialFlags(uint kind)
        {
            if (kind == P2bMeshExporter.KindLitAlpha ||
                kind == P2bMeshExporter.KindAdditive)
                return 2u | 4u; // blend, transparent, no Z write
            return 1u; // opaque: Z write
        }

        private static byte[] BuildScene(List<EntityRecord> entities,
                                         Dictionary<string, uint> scriptNames)
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
                    var cam = e.Camera;
                    p.F32(cam.fieldOfView * Mathf.Deg2Rad);
                    p.F32(cam.nearClipPlane);
                    p.F32(cam.farClipPlane);
                    // M8 camera payload (64 bytes; readers accept the old 12).
                    p.U32(cam.orthographic ? 1u : 0u);
                    p.F32(cam.orthographicSize);
                    p.F32(cam.rect.x);
                    p.F32(cam.rect.y);
                    p.F32(cam.rect.width);
                    p.F32(cam.rect.height);
                    p.U32(cam.clearFlags == CameraClearFlags.Depth ? 2u : 1u);
                    Color32 cc = cam.backgroundColor;
                    p.U32((uint)cc.r | ((uint)cc.g << 8) | ((uint)cc.b << 16));
                    p.U32((uint)cam.cullingMask);
                    p.U32(RenderSettings.fog ? 1u : 0u);
                    Color32 fc = RenderSettings.fogColor;
                    p.U32((uint)fc.r | ((uint)fc.g << 8) | ((uint)fc.b << 16));
                    p.F32(RenderSettings.fogStartDistance);
                    p.F32(RenderSettings.fogEndDistance);
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
                if (e.Skinned != null)
                {
                    var p = new ByteBuffer();
                    int group;
                    if (!PendingSkin.RendererGroup.TryGetValue(e.Skinned, out group))
                        group = 0;
                    p.U32((uint)PendingSkin.RendererMesh[e.Skinned]);
                    p.U32(0xFFFFFFFF); // no material override
                    p.U32((uint)group); // which character -> which animator
                    p.U32(0);          // controller index
                    comps.Add((5, p.ToArray()));
                }
                if (e.Animator != null && PendingSkin != null)
                {
                    // 8 bytes: controller index and the layer count the
                    // exporter actually baked. Only entities with a rig get
                    // one -- an Animator with nothing to drive would bind to
                    // controller 0 of an empty table.
                    var p = new ByteBuffer();
                    p.U32(0); // controller index
                    p.U32(1); // layers baked
                    comps.Add((7, p.ToArray()));
                }
                else if (e.Animator != null)
                {
                    // Dropping a component the scene visibly has, silently,
                    // is the defect class M12.5 exists to kill: it surfaced
                    // as GetComponent<Animator>() returning null on target
                    // with nothing anywhere saying why (verify-log M12.5).
                    Debug.LogWarning(
                        "[PS2] '" + e.Transform.name + "' has an Animator but " +
                        "the scene exported no rig, so the component is " +
                        "dropped: GetComponent<Animator>() will return null " +
                        "on target. A rig needs at least one " +
                        "SkinnedMeshRenderer with bones -- a model imported " +
                        "with Rig set to None, or a static copy of the " +
                        "character, has none.");
                }
                if (e.Body != null)
                {
                    // 16 bytes: mass, the two damping terms, and the flags the
                    // solver actually reads. Deliberately NOT here: constraints
                    // beyond freezeRotation, interpolation, collision detection
                    // mode and centre of mass -- this solver has no equivalent
                    // for any of them (ADR-009), and exporting a field nothing
                    // consumes is how a value silently stops meaning anything.
                    var p = new ByteBuffer();
                    p.F32(e.Body.mass);
                    p.F32(e.Body.linearDamping);
                    p.F32(e.Body.angularDamping);
                    uint flags = 0;
                    if (e.Body.useGravity) flags |= 1u;
                    if (e.Body.isKinematic) flags |= 2u;
                    if (e.Body.freezeRotation) flags |= 4u;
                    p.U32(flags);
                    comps.Add((6, p.ToArray()));
                }
                if (e.Scripts != null)
                {
                    foreach (string s in e.Scripts)
                    {
                        var p = new ByteBuffer();
                        p.U32(scriptNames[s]); // offset into SCRP
                        comps.Add((4, p.ToArray()));
                    }
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
                b.U16((ushort)t.gameObject.layer);
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

        // ---- M7 acceptance: the Spin scene + Editor golden trace -----------
        //
        // Batch entry:
        //   Unity -batchmode -quit -executeMethod Ps2.Editor.PS2ExportMenu.ExportSpinScene -ps2Output <scene.p2b>
        //
        // Builds a cube carrying the user's Spin + SpinParityCheck scripts
        // (which must exist in Assets/ -- they are USER code), exports the
        // scene, and generates Assets/PS2Scripts/SpinGolden.cs: 300 frames of
        // the cube's localToWorldMatrix as REAL Unity computes it, with the
        // same fixed dt the PS2 main loop uses. The golden is compiled into
        // the game assembly, so the on-target parity check needs no file I/O.
        public static void ExportSpinScene()
        {
            string output = "spin-scene.p2b";
            string[] args = System.Environment.GetCommandLineArgs();
            for (int i = 0; i < args.Length - 1; i++)
            {
                if (args[i] == "-ps2Output")
                {
                    output = args[i + 1];
                }
            }

            const float dt = 1.0f / 30.0f;
            const int frames = 300;
            Vector3 cubePosition = new Vector3(0, 0.5f, 3.0f);

            // Golden first (independent simulation object, real Unity math).
            var sim = new GameObject("golden-sim");
            sim.transform.position = cubePosition;
            var sb = new System.Text.StringBuilder(64 * 1024);
            sb.Append("// GENERATED by PS2ExportMenu.ExportSpinScene -- 300 frames of\n");
            sb.Append("// localToWorldMatrix from Unity's own Transform, dt = 1/30.\n");
            sb.Append("// The on-target parity check compares the shim against this.\n");
            sb.Append("public static class SpinGolden\n{\n");
            sb.Append("    public static readonly float[] Data = new float[]\n    {\n");
            for (int f = 0; f < frames; f++)
            {
                sim.transform.Rotate(0f, 90f * dt, 0f);
                Matrix4x4 m = sim.transform.localToWorldMatrix;
                sb.Append("        ");
                for (int i = 0; i < 16; i++)
                {
                    sb.Append(m[i].ToString("R",
                        System.Globalization.CultureInfo.InvariantCulture));
                    sb.Append("f, ");
                }
                sb.Append("\n");
            }
            sb.Append("    };\n}\n");
            Object.DestroyImmediate(sim);

            string goldenPath = System.IO.Path.Combine(
                Application.dataPath, "PS2Scripts", "SpinGolden.cs");
            System.IO.Directory.CreateDirectory(
                System.IO.Path.GetDirectoryName(goldenPath));
            System.IO.File.WriteAllText(goldenPath, sb.ToString());
            AssetDatabase.Refresh();

            // Scene: camera at origin looking +z, one lit backdrop, the cube.
            var scene = SceneManager.GetActiveScene();
            foreach (GameObject root in scene.GetRootGameObjects())
            {
                Object.DestroyImmediate(root);
            }

            var camGo = new GameObject("Camera");
            var cam = camGo.AddComponent<Camera>();
            cam.fieldOfView = 60.0f;
            cam.nearClipPlane = 0.5f;
            cam.farClipPlane = 100.0f;
            camGo.transform.position = Vector3.zero;

            var lightGo = new GameObject("Sun");
            var light = lightGo.AddComponent<Light>();
            light.type = LightType.Directional;
            light.color = Color.white;
            light.intensity = 0.9f;
            lightGo.transform.rotation = Quaternion.Euler(50.0f, -30.0f, 0);

            var cube = GameObject.CreatePrimitive(PrimitiveType.Cube);
            cube.name = "spincube";
            var cubeMat = new Material(Shader.Find("Unlit/Color"));
            cubeMat.color = new Color(0.9f, 0.6f, 0.2f, 1.0f);
            cube.GetComponent<MeshRenderer>().sharedMaterial = cubeMat;
            cube.transform.position = cubePosition;

            var spinType = FindUserType("Spin");
            var parityType = FindUserType("SpinParityCheck");
            if (spinType == null || parityType == null)
            {
                Debug.LogError("[PS2] Spin/SpinParityCheck not found in " +
                               "Assembly-CSharp; are the scripts in Assets/?");
                EditorApplication.Exit(1);
                return;
            }
            cube.AddComponent(spinType);
            cube.AddComponent(parityType);

            P2bSceneExporter.ExportActiveScene(output);
            Debug.Log("[PS2] spin scene exported to " + output);
        }

        // ---- M8 acceptance: the 500-object scene-graph scene ---------------
        //
        // Batch entry:
        //   Unity -batchmode -quit -executeMethod Ps2.Editor.PS2ExportMenu.ExportSceneGraphScene -ps2Output <scene.p2b>
        //
        // 25 towers x 20 cubes = 500 mesh objects in 20-deep parent chains
        // (each cube is the child of the one below), five material kinds:
        // VertexLit, UnlitTextured, LitAlpha, Cutout, Additive. Two
        // translucent towers sit near the camera line so back-to-front
        // sorting is visible in the goldens. Deterministic by construction:
        // no Random, no time.
        public static void ExportSceneGraphScene()
        {
            string output = "scenegraph.p2b";
            string[] args = System.Environment.GetCommandLineArgs();
            for (int i = 0; i < args.Length - 1; i++)
            {
                if (args[i] == "-ps2Output")
                {
                    output = args[i + 1];
                }
            }

            var scene = SceneManager.GetActiveScene();
            foreach (GameObject root in scene.GetRootGameObjects())
            {
                Object.DestroyImmediate(root);
            }

            var camGo = new GameObject("Camera");
            var cam = camGo.AddComponent<Camera>();
            cam.fieldOfView = 60.0f;
            cam.nearClipPlane = 0.5f;
            cam.farClipPlane = 150.0f;
            cam.backgroundColor = new Color(24 / 255f, 28 / 255f, 44 / 255f, 1f);
            camGo.transform.position = new Vector3(0, 6.0f, -30.0f);

            var lightGo = new GameObject("Sun");
            var light = lightGo.AddComponent<Light>();
            light.type = LightType.Directional;
            light.color = Color.white;
            light.intensity = 0.9f;
            lightGo.transform.rotation = Quaternion.Euler(50.0f, -30.0f, 0);

            // One checkerboard texture for the textured towers.
            var tex = new Texture2D(64, 64, TextureFormat.RGBA32, false);
            var px = new Color32[64 * 64];
            for (int y = 0; y < 64; y++)
            {
                for (int x = 0; x < 64; x++)
                {
                    bool check = (((x >> 3) + (y >> 3)) & 1) == 0;
                    px[y * 64 + x] = check ? new Color32(230, 200, 60, 255)
                                           : new Color32(40, 60, 120, 255);
                }
            }
            tex.SetPixels32(px);
            tex.Apply();
            tex.name = "sgtex";

            var texturedMat = new Material(Shader.Find("Unlit/Texture"));
            texturedMat.mainTexture = tex;

            var litMat = new Material(Shader.Find("Unlit/Color"));
            litMat.color = new Color(0.85f, 0.3f, 0.25f, 1f);

            var alphaMat = new Material(Shader.Find("Unlit/Color"));
            alphaMat.color = new Color(0.3f, 0.5f, 0.95f, 0.45f);
            alphaMat.SetOverrideTag("RenderType", "Transparent");
            alphaMat.renderQueue = 3000;

            var cutoutMat = new Material(Shader.Find("Unlit/Color"));
            cutoutMat.color = new Color(0.35f, 0.8f, 0.35f, 1f);
            cutoutMat.SetOverrideTag("RenderType", "TransparentCutout");

            // "Additive" is classified by shader NAME; Unlit/Color under a
            // renamed dummy shader would not survive a build, so use the
            // legacy particle additive shader when present and fall back to
            // tagging via name match on Unlit/Color otherwise.
            Shader additiveShader = Shader.Find("Legacy Shaders/Particles/Additive");
            Material additiveMat;
            if (additiveShader != null)
            {
                additiveMat = new Material(additiveShader);
                additiveMat.color = new Color(0.9f, 0.7f, 0.3f, 0.6f);
            }
            else
            {
                additiveMat = new Material(Shader.Find("Unlit/Color"));
                additiveMat.color = new Color(0.9f, 0.7f, 0.3f, 0.6f);
                additiveMat.SetOverrideTag("RenderType", "Transparent");
                additiveMat.renderQueue = 3000;
            }

            Material[] cycle = { litMat, texturedMat, alphaMat, cutoutMat, additiveMat };

            for (int tower = 0; tower < 25; tower++)
            {
                int gx = tower % 5;
                int gz = tower / 5;
                // The two translucent towers land in front of the camera
                // line (x=0) at different depths: sorting must order them.
                float baseX = (gx - 2) * 8.0f;
                float baseZ = gz * 8.0f;
                Material mat = cycle[tower % 5];

                Transform parent = null;
                for (int level = 0; level < 20; level++)
                {
                    var go = GameObject.CreatePrimitive(PrimitiveType.Cube);
                    go.name = "t" + tower + "_l" + level;
                    go.GetComponent<MeshRenderer>().sharedMaterial = mat;
                    if (parent == null)
                    {
                        go.transform.position = new Vector3(baseX, 0.0f, baseZ);
                    }
                    else
                    {
                        go.transform.SetParent(parent, false);
                        go.transform.localPosition = new Vector3(0, 1.05f, 0);
                        go.transform.localRotation = Quaternion.Euler(0, 7.0f, 0);
                        go.transform.localScale = Vector3.one * 0.97f;
                    }
                    parent = go.transform;
                }
            }

            P2bSceneExporter.ExportActiveScene(output);
            Debug.Log("[PS2] scene-graph scene exported to " + output);
        }

        // ---- M8 task 7: fog verification scene -----------------------------
        //
        //   Unity -batchmode -quit -executeMethod Ps2.Editor.PS2ExportMenu.ExportFogScene -ps2Output <scene.p2b>
        //
        // Linear fog on, five lit cubes at increasing depth. The runtime
        // sample projects each cube's centre and asserts the rendered colour
        // slides monotonically toward the fog colour with distance.
        public static void ExportFogScene()
        {
            string output = "fogscene.p2b";
            string[] args = System.Environment.GetCommandLineArgs();
            for (int i = 0; i < args.Length - 1; i++)
            {
                if (args[i] == "-ps2Output")
                {
                    output = args[i + 1];
                }
            }

            var scene = SceneManager.GetActiveScene();
            foreach (GameObject root in scene.GetRootGameObjects())
            {
                Object.DestroyImmediate(root);
            }

            RenderSettings.fog = true;
            RenderSettings.fogMode = FogMode.Linear;
            RenderSettings.fogColor = new Color(120 / 255f, 140 / 255f, 180 / 255f, 1f);
            RenderSettings.fogStartDistance = 10.0f;
            RenderSettings.fogEndDistance = 60.0f;

            var camGo = new GameObject("Camera");
            var cam = camGo.AddComponent<Camera>();
            cam.fieldOfView = 60.0f;
            cam.nearClipPlane = 0.5f;
            cam.farClipPlane = 120.0f;
            cam.backgroundColor = new Color(24 / 255f, 28 / 255f, 44 / 255f, 1f);
            camGo.transform.position = Vector3.zero;

            var lightGo = new GameObject("Sun");
            var light = lightGo.AddComponent<Light>();
            light.type = LightType.Directional;
            light.color = Color.white;
            light.intensity = 0.9f;
            lightGo.transform.rotation = Quaternion.Euler(50.0f, -30.0f, 0);

            var mat = new Material(Shader.Find("Unlit/Color"));
            mat.color = new Color(0.9f, 0.35f, 0.25f, 1f);

            float[] depths = { 8f, 20f, 35f, 50f, 75f };
            float[] lateral = { -6f, -3f, 0f, 3f, 6f };
            for (int i = 0; i < 5; i++)
            {
                var go = GameObject.CreatePrimitive(PrimitiveType.Cube);
                go.name = "fogcube" + i;
                go.GetComponent<MeshRenderer>().sharedMaterial = mat;
                go.transform.position = new Vector3(
                    lateral[i] * (depths[i] / 12.0f), 0f, depths[i]);
                go.transform.localScale = Vector3.one * (depths[i] / 8.0f);
            }

            P2bSceneExporter.ExportActiveScene(output);
            RenderSettings.fog = false; // leave the editor state clean
            Debug.Log("[PS2] fog scene exported to " + output);
        }

        private static System.Type FindUserType(string name)
        {
            foreach (var asm in System.AppDomain.CurrentDomain.GetAssemblies())
            {
                if (!asm.GetName().Name.StartsWith("Assembly-CSharp")) continue;
                var t = asm.GetType(name);
                if (t != null) return t;
            }
            return null;
        }
    }
}
