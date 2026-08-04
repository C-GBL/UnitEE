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
            public AudioSource Audio;           // M12.5 task 2, or null
            public bool Listener;               // has an AudioListener
            public Ps2.Runtime.PS2ParticleSystem Particles; // task 4, or null
            public UIRecord Ui;                 // task 5, or null
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
            // Group index -> the transform whose ENTITY the group's renderer
            // records bind to (the Animator, or the character root). The
            // rest pose and every clip key are exported relative to it, so
            // the runtime's palette-times-entity-world composition is exact
            // whatever non-bone nodes the FBX parked in between.
            public List<Transform> GroupAnimators = new List<Transform>();
            // Ground-truth measurements taken IN Unity at export: written
            // next to the .p2b as <name>.rigdiag.json. When a character
            // draws wrong on target, this answers which side lied without
            // another guess-rebuild round trip.
            public List<string> Diagnosis = new List<string>();
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

        // The profile's audio sample rate, set by the build step. 22050 is
        // the plan's SFX rate and the profile default.
        internal static int AudioSampleRate = 22050;

        // Pre-built SND section (M10), supplied by the audio export
        // entry point the same way PendingSkin supplies the rig.
        internal static byte[] PendingSound;

        // One uGUI element (M12.5 task 5), captured during the walk with its
        // screen rect already resolved -- real RectTransform anchor math
        // against the profile framebuffer, baked at export (deviation 30).
        internal sealed class UIRecord
        {
            public int DrawKind;      // 0 rect, 1 image, 2 text
            public int ManagedKind;   // 0 Image, 1 RawImage, 2 Text
            public float X, Y, W, H;  // screen px, top-left origin
            public Color Colour = Color.white;
            public Texture2D Texture;
            public string Text = "";
            public int TextScale = 1;
            public int Role;          // 0 none, 1 button, 2 slider
            public Transform SliderFill;
            public float SliderValue;
            public float SliderMaxW;
            public Vector4 Border;    // 9-slice L,B,R,T (sprite.border px)
            public int AlignH;        // 0 left, 1 centre, 2 right
            public int AlignV;        // 0 top, 1 middle, 2 bottom
            public Font UsedFont;     // Text only; baked per (font, size)
            public int UsedFontSize;
            public int FontIndex = -1; // FONT table index, -1 = builtin 8x8
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

            // Particle textures (M12.5 task 4): registered after the walk
            // found the components, deduplicated against everything else.
            foreach (var e in entities)
            {
                Texture2D fx = e.Particles != null ? e.Particles.texture : null;
                if (fx != null && !textureLookup.ContainsKey(fx))
                {
                    textureLookup[fx] = textures.Count;
                    textures.Add(fx);
                }
                Texture2D ui = e.Ui != null ? e.Ui.Texture : null;
                if (ui != null && !textureLookup.ContainsKey(ui))
                {
                    textureLookup[ui] = textures.Count;
                    textures.Add(ui);
                }
            }

            // Baked Unity fonts (M12.5): one atlas per (font, size) pair the
            // scene's Texts use, rasterised by Unity's own font engine. The
            // atlas rides the ordinary texture path; the FONT section holds
            // the metrics. Capped at the runtime's four slots; overflow and
            // bake failures fall back to the builtin 8x8 font, said aloud.
            var bakedFonts = new List<P2bFontExporter.BakedFont>();
            var fontIndexOf = new Dictionary<string, int>();
            foreach (var e in entities)
            {
                UIRecord ui = e.Ui;
                if (ui == null || ui.ManagedKind != 2 || ui.UsedFont == null)
                    continue;
                string fontKey =
                    ui.UsedFont.GetInstanceID() + ":" + ui.UsedFontSize;
                if (!fontIndexOf.TryGetValue(fontKey, out int fi))
                {
                    fi = -1;
                    if (bakedFonts.Count >= 4)
                    {
                        Debug.LogWarning(
                            "[PS2] more than 4 (font, size) pairs in the " +
                            "scene; '" + e.Transform.name + "' falls back " +
                            "to the builtin font.");
                    }
                    else
                    {
                        var fontWarnings = new List<string>();
                        P2bFontExporter.BakedFont bakedFont =
                            P2bFontExporter.Bake(ui.UsedFont, ui.UsedFontSize,
                                                 fontWarnings);
                        foreach (string w in fontWarnings)
                            Debug.LogWarning("[PS2] " + w);
                        if (bakedFont != null)
                        {
                            fi = bakedFonts.Count;
                            bakedFonts.Add(bakedFont);
                        }
                    }
                    fontIndexOf[fontKey] = fi;
                }
                ui.FontIndex = fi;
            }

            // AudioClips referenced by the scene's AudioSources -> SND (M12.5
            // task 2). A clip is encoded LOOPED when its source loops: SPU2
            // loop points live in the ADPCM stream itself, so looping is a
            // clip property here, and one asset used both ways is simply
            // encoded twice.
            var audioClipIndex = new Dictionary<string, int>();
            bool autoBakedSound = false;
            if (PendingSound == null)
            {
                var encodedClips = new List<P2bAudioExporter.EncodedClip>();
                foreach (var e in entities)
                {
                    AudioSource source = e.Audio;
                    if (source == null || source.clip == null)
                        continue;
                    string key = source.clip.GetInstanceID() + ":" + source.loop;
                    if (audioClipIndex.ContainsKey(key))
                        continue;
                    try
                    {
                        encodedClips.Add(P2bAudioExporter.Encode(
                            source.clip, source.loop, AudioSampleRate));
                        audioClipIndex[key] = encodedClips.Count - 1;
                    }
                    catch (System.Exception ex)
                    {
                        Debug.LogWarning(
                            "[PS2] AudioClip '" + source.clip.name + "' could not " +
                            "be encoded (" + ex.Message + "). Set its Load Type " +
                            "to Decompress On Load; the source exports with no " +
                            "clip until then.");
                    }
                }
                if (encodedClips.Count > 0)
                {
                    PendingSound = P2bAudioExporter.BuildSection(encodedClips);
                    autoBakedSound = true;
                }
            }
            else
            {
                foreach (var e in entities)
                {
                    if (e.Audio != null && e.Audio.clip != null)
                    {
                        Debug.LogWarning(
                            "[PS2] the SND section was supplied by an export menu, " +
                            "so scene AudioSources cannot index into it; their " +
                            "clips are dropped for this export.");
                        break;
                    }
                }
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
            // Font atlases as TEX sections AFTER the art (their indices
            // follow it), encoded directly -- known palette, no quantiser,
            // no downscale: every glyph metric is a pixel coordinate.
            for (int k = 0; k < bakedFonts.Count; k++)
            {
                writer.AddSection(P2bWriter.SectionTex,
                                  P2bFontExporter.BuildAtlasSection(bakedFonts[k]),
                                  "font-atlas-" + k);
                writer.AddSection(
                    P2bWriter.SectionFont,
                    P2bFontExporter.BuildSection(bakedFonts[k],
                                                 textures.Count + k));
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

            writer.AddSection(P2bWriter.SectionScene, BuildScene(entities, scriptNames, audioClipIndex,
                                       textureLookup));
            writer.Write(path);
            if (PendingSkin != null && PendingSkin.Diagnosis.Count > 0)
            {
                string diagPath = path + ".rigdiag.json";
                System.IO.File.WriteAllLines(diagPath, PendingSkin.Diagnosis);
                Debug.Log("[PS2] rig diagnosis written to " + diagPath);
            }

            // A rig this call baked belongs to THIS scene. PendingSkin is
            // static, so leaving it set would carry scene 1's character into
            // scene 2 of a multi-scene build -- the same shape as the M10 bug
            // where World::load did not reset its animation counters and every
            // third load overflowed them. A rig a CALLER supplied is theirs to
            // clear, and PS2SkinExportMenu does.
            if (autoBakedRig)
                PendingSkin = null;
            if (autoBakedSound)
                PendingSound = null;
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
            record.Audio = t.GetComponent<AudioSource>();
            record.Listener = t.GetComponent<AudioListener>() != null;
            record.Particles = t.GetComponent<Ps2.Runtime.PS2ParticleSystem>();
            record.Ui = CaptureUI(t);

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

        // Resolves a RectTransform chain to screen pixels against the
        // framebuffer -- the real anchor/offset math, so authored layouts
        // survive exactly; only the CanvasScaler is ignored (deviation 30:
        // the reference resolution IS the framebuffer).
        private static Rect ResolveScreenRect(RectTransform rt, float screenW,
                                              float screenH)
        {
            var chain = new List<RectTransform>();
            for (RectTransform r = rt; r != null; r = r.parent as RectTransform)
            {
                if (r.GetComponent<Canvas>() != null)
                    break;
                chain.Add(r);
            }
            var rect = new Rect(0, 0, screenW, screenH);
            for (int i = chain.Count - 1; i >= 0; i--)
            {
                RectTransform r = chain[i];
                float xMin = rect.xMin + r.anchorMin.x * rect.width + r.offsetMin.x;
                float xMax = rect.xMin + r.anchorMax.x * rect.width + r.offsetMax.x;
                float yMin = rect.yMin + r.anchorMin.y * rect.height + r.offsetMin.y;
                float yMax = rect.yMin + r.anchorMax.y * rect.height + r.offsetMax.y;
                rect = new Rect(xMin, yMin, xMax - xMin, yMax - yMin);
            }
            return rect;
        }

        private static UIRecord CaptureUI(Transform t)
        {
            if (t.GetComponentInParent<Canvas>() == null)
                return null;
            var rt = t as RectTransform;

            float screenW = 512f, screenH = 448f; // PS2 framebuffer default
            var record = new UIRecord();
            bool drawable = false;

            var text = t.GetComponent<UnityEngine.UI.Text>();
            var image = t.GetComponent<UnityEngine.UI.Image>();
            var raw = t.GetComponent<UnityEngine.UI.RawImage>();
            if (text != null)
            {
                record.DrawKind = 2;
                record.ManagedKind = 2;
                record.Colour = text.color;
                record.Text = text.text ?? "";
                if (record.Text.Length > 47)
                {
                    Debug.LogWarning(
                        "[PS2] Text on '" + t.name + "' is over the 48-byte " +
                        "card of the baked font path and was truncated. " +
                        "Deviation 30 in docs/supported-api.md.");
                    record.Text = record.Text.Substring(0, 47);
                }
                record.TextScale =
                    Mathf.Clamp(Mathf.RoundToInt(text.fontSize / 8f), 1, 4);
                // TextAnchor enumerates a 3x3 grid row-major (UpperLeft=0
                // .. LowerRight=8), so column and row fall out of div/mod.
                record.AlignH = (int)text.alignment % 3;
                record.AlignV = (int)text.alignment / 3;
                // The REAL font, baked later per (font, size). TextScale
                // above survives as the fallback path's size.
                record.UsedFont = text.font;
                record.UsedFontSize = Mathf.Max(1, text.fontSize);
                drawable = true;
            }
            else if (image != null)
            {
                record.Colour = image.color;
                record.Texture =
                    image.sprite != null ? image.sprite.texture : null;
                record.DrawKind = record.Texture != null ? 1 : 0;
                record.ManagedKind = 0;
                drawable = true;
                if (image.sprite != null &&
                    (image.type == UnityEngine.UI.Image.Type.Sliced ||
                     image.type == UnityEngine.UI.Image.Type.Tiled))
                {
                    // sprite.border is (L, B, R, T) in sprite pixels; the
                    // runtime draws real 9-slice from it. Tiled gets the
                    // sliced look -- the closest this hardware path has.
                    record.Border = image.sprite.border;
                }
                if (image.sprite != null && record.Texture != null &&
                    (image.sprite.rect.width < record.Texture.width ||
                     image.sprite.rect.height < record.Texture.height))
                {
                    Debug.LogWarning(
                        "[PS2] Image on '" + t.name + "' uses a sub-rect of " +
                        "a sprite atlas; the PS2 path draws the sprite's " +
                        "WHOLE texture (no 9-slice, no atlas UVs -- " +
                        "deviation 30). Give it a standalone sprite.");
                }
            }
            else if (raw != null)
            {
                record.Colour = raw.color;
                record.Texture = raw.texture as Texture2D;
                record.DrawKind = record.Texture != null ? 1 : 0;
                record.ManagedKind = 1;
                drawable = true;
            }

            var button = t.GetComponent<UnityEngine.UI.Button>();
            var slider = t.GetComponent<UnityEngine.UI.Slider>();
            if (!drawable && button == null && slider == null)
                return null;

            if (rt != null)
            {
                Rect r = ResolveScreenRect(rt, screenW, screenH);
                record.X = r.xMin;
                record.Y = screenH - r.yMax; // GS is top-left, y down
                record.W = r.width;
                record.H = r.height;
            }
            if (!drawable)
            {
                // A Button/Slider with no graphic of its own: a zero-size
                // role-only record that never draws.
                record.W = 0;
                record.H = 0;
            }
            if (button != null)
            {
                record.Role = 1;
            }
            else if (slider != null)
            {
                record.Role = 2;
                record.SliderFill =
                    slider.fillRect != null ? slider.fillRect.transform : null;
                record.SliderValue =
                    slider.maxValue > slider.minValue
                        ? (slider.value - slider.minValue) /
                              (slider.maxValue - slider.minValue)
                        : 0f;
                if (record.SliderFill != null)
                {
                    Rect fr = ResolveScreenRect(
                        (RectTransform)record.SliderFill, screenW, screenH);
                    record.SliderMaxW =
                        record.SliderValue > 0.01f
                            ? fr.width / record.SliderValue
                            : fr.width;
                }
            }
            return record;
        }

        private static uint PackColour(Color c)
        {
            uint r = (uint)Mathf.Clamp(Mathf.RoundToInt(c.r * 255f), 0, 255);
            uint g = (uint)Mathf.Clamp(Mathf.RoundToInt(c.g * 255f), 0, 255);
            uint b = (uint)Mathf.Clamp(Mathf.RoundToInt(c.b * 255f), 0, 255);
            uint a = (uint)Mathf.Clamp(Mathf.RoundToInt(c.a * 255f), 0, 255);
            return r | (g << 8) | (b << 16) | (a << 24);
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
                                         Dictionary<string, uint> scriptNames,
                                         Dictionary<string, int> audioClipIndex,
                                         Dictionary<Texture2D, int> textureLookup)
        {
            // uGUI element indices in entity (hierarchy) order -- the draw
            // order -- assigned up front so a Slider can name its fill.
            var uiIndexOf = new Dictionary<Transform, int>();
            foreach (var e in entities)
            {
                if (e.Ui != null)
                    uiIndexOf[e.Transform] = uiIndexOf.Count;
            }

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
                // Skinned renderer records live on the entity the rig's rest
                // pose and clips were exported RELATIVE TO -- the Animator's
                // entity -- because the runtime multiplies the palette by
                // that entity's world matrix. They used to sit on each
                // SkinnedMeshRenderer node, and an FBX whose SMR nodes carry
                // import scaling (x100 is routine) drew the character in the
                // wrong space entirely: submitted every frame, every
                // triangle behind the near plane (verify-log M12.5).
                if (PendingSkin != null)
                {
                    for (int g = 0; g < PendingSkin.GroupAnimators.Count; g++)
                    {
                        if (PendingSkin.GroupAnimators[g] != e.Transform)
                            continue;
                        foreach (var kv in PendingSkin.RendererGroup)
                        {
                            if (kv.Value != g)
                                continue;
                            var p = new ByteBuffer();
                            p.U32((uint)PendingSkin.RendererMesh[kv.Key]);
                            p.U32(0xFFFFFFFF); // no material override
                            p.U32((uint)g);    // which character -> animator
                            p.U32(0);          // controller index
                            comps.Add((5, p.ToArray()));
                        }
                    }
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
                if (e.Audio != null)
                {
                    // 24 bytes: clip, volume, flags, min/max distance,
                    // priority. Priority crosses to the mixer's higher-wins
                    // scale here, once, so neither side ever guesses.
                    AudioSource src = e.Audio;
                    uint clipIndex = 0xFFFFFFFF;
                    if (src.clip != null)
                    {
                        string key = src.clip.GetInstanceID() + ":" + src.loop;
                        if (audioClipIndex.TryGetValue(key, out int ci))
                            clipIndex = (uint)ci;
                    }
                    var p = new ByteBuffer();
                    p.U32(clipIndex);
                    p.F32(src.volume);
                    uint f = 0;
                    if (src.playOnAwake) f |= 1;
                    if (src.loop) f |= 2;
                    if (src.spatialBlend > 0.5f) f |= 4;
                    p.U32(f);
                    p.F32(src.minDistance);
                    p.F32(src.maxDistance);
                    p.U32((uint)Mathf.Clamp(256 - src.priority, 0, 256));
                    comps.Add((8, p.ToArray()));
                }
                if (e.Listener)
                {
                    var p = new ByteBuffer();
                    p.U32(0); // 4 bytes, all pad
                    comps.Add((9, p.ToArray()));
                }
                if (e.Particles != null)
                {
                    // 64 bytes; ParticleEmitter field order (ADR-011).
                    Ps2.Runtime.PS2ParticleSystem fx = e.Particles;
                    var p = new ByteBuffer();
                    uint tex = 0xFFFFFFFF;
                    if (fx.texture != null &&
                        textureLookup.TryGetValue(fx.texture, out int ti))
                        tex = (uint)ti;
                    p.U32(tex);
                    uint f = 0;
                    if (fx.looping) f |= 1;
                    if (fx.playOnAwake) f |= 2;
                    if (fx.additive) f |= 4;
                    if (fx.worldSpace) f |= 8;
                    p.U32(f);
                    p.F32(fx.emissionRate);
                    p.U32((uint)Mathf.Max(0, fx.burstCount));
                    p.U32((uint)fx.shape);
                    p.F32(fx.shapeA);
                    p.F32(fx.shapeB);
                    p.F32(fx.shapeC);
                    p.F32(fx.lifetime);
                    p.F32(fx.speed);
                    p.F32(fx.sizeStart);
                    p.F32(fx.sizeEnd);
                    p.U32(PackColour(fx.colourStart));
                    p.U32(PackColour(fx.colourEnd));
                    p.F32(fx.gravityModifier);
                    p.U32((uint)Mathf.Clamp(fx.maxParticles, 1, 128));
                    comps.Add((10, p.ToArray()));
                }
                if (e.Ui != null)
                {
                    // 84 bytes (M12.5 task 5): kind|managed<<8 in the low
                    // u16, role in the high u16, baked screen rect, tint
                    // (alpha in the PS2 0..0x80 range), texture, slider fill
                    // link, text scale, then 48 bytes of text -- which for a
                    // slider carry (f32 value, f32 max fill width) instead.
                    UIRecord ui = e.Ui;
                    var p = new ByteBuffer();
                    p.U32((uint)(ui.DrawKind | (ui.ManagedKind << 8) |
                                 (ui.Role << 16)));
                    p.F32(ui.X);
                    p.F32(ui.Y);
                    p.F32(ui.W);
                    p.F32(ui.H);
                    uint cr = (uint)Mathf.Clamp(Mathf.RoundToInt(ui.Colour.r * 255f), 0, 255);
                    uint cg = (uint)Mathf.Clamp(Mathf.RoundToInt(ui.Colour.g * 255f), 0, 255);
                    uint cb = (uint)Mathf.Clamp(Mathf.RoundToInt(ui.Colour.b * 255f), 0, 255);
                    uint ca = (uint)Mathf.Clamp(Mathf.RoundToInt(ui.Colour.a * 128f), 0, 128);
                    p.U32(cr | (cg << 8) | (cb << 16) | (ca << 24));
                    uint uiTex = 0xFFFFFFFF;
                    if (ui.Texture != null &&
                        textureLookup.TryGetValue(ui.Texture, out int uti))
                        uiTex = (uint)uti;
                    p.U32(uiTex);
                    int link = -1;
                    if (ui.SliderFill != null &&
                        uiIndexOf.TryGetValue(ui.SliderFill, out int fi))
                        link = fi;
                    p.U32((uint)link);
                    // Low byte scale; bits 8-9 / 10-11 the Text alignment;
                    // bits 16-23 the FONT index plus one (0 = builtin 8x8).
                    p.U32((uint)(ui.TextScale | (ui.AlignH << 8) |
                                 (ui.AlignV << 10) |
                                 ((ui.FontIndex + 1) << 16)));
                    if (ui.Role == 2)
                    {
                        p.F32(ui.SliderValue);
                        p.F32(ui.SliderMaxW);
                        for (int tb = 8; tb < 48; tb++) p.U8(0);
                    }
                    else if (ui.DrawKind == 1)
                    {
                        // Images carry no text, so the text bytes carry the
                        // 9-slice borders instead: four f32, runtime order
                        // L, T, R, B (sprite.border is L, B, R, T). Zeros =
                        // Image.Type.Simple = plain stretch. A slider
                        // background that is ALSO an image keeps its
                        // value/maxW above and loses its border.
                        p.F32(ui.Border.x); // left
                        p.F32(ui.Border.w); // top
                        p.F32(ui.Border.z); // right
                        p.F32(ui.Border.y); // bottom
                        for (int tb = 16; tb < 48; tb++) p.U8(0);
                    }
                    else
                    {
                        var bytes = System.Text.Encoding.ASCII.GetBytes(ui.Text ?? "");
                        for (int tb = 0; tb < 48; tb++)
                            p.U8(tb < bytes.Length && tb < 47 ? bytes[tb]
                                                              : (byte)0);
                    }
                    comps.Add((11, p.ToArray()));
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
