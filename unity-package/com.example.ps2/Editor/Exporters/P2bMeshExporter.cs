// Mesh -> MESH section (plan section 9 M5 task 2; layout per
// docs/formats/p2b-container.md and runtime/include/ps2ur/gs_batch.h).
//
// Deindexes the triangle list and splits it into VU1-sized batch blobs whose
// bytes are exactly what BatchBuilder produces at runtime, so the console
// references them zero-copy: [GIF tag qw][count qw][vertex qws] per batch.
using System;
using UnityEngine;

namespace Ps2.Editor
{
    internal static class P2bMeshExporter
    {
        public const int MaxUnlitVertsPerBatch = 93;
        public const int MaxTexVertsPerBatch = 78;
        public const int MaxLitVertsPerBatch = 78;

        // Material kinds, matching the runtime's p2b_scene.h (plan 7.3).
        public const uint KindUnlit = 0;
        public const uint KindUnlitTextured = 1;
        public const uint KindVertexLit = 2;
        public const uint KindLitAlpha = 3; // lit layout + blend, no Z write
        public const uint KindCutout = 4;   // lit layout + alpha test
        public const uint KindAdditive = 5; // unlit layout + additive blend
        public const uint KindVertexLitFog = 6; // lit layout + per-vertex F (M8)
        public const uint KindSkinned = 7;      // vu_skin palette (M9)

        // Vertex layout selectors: the new kinds reuse the M4/M5 layouts.
        public static bool UsesLitLayout(uint kind) =>
            kind == KindVertexLit || kind == KindLitAlpha || kind == KindCutout ||
            kind == KindVertexLitFog;
        public static bool UsesTexLayout(uint kind) => kind == KindUnlitTextured;
        public static bool UsesBlend(uint kind) =>
            kind == KindLitAlpha || kind == KindAdditive;

        // The VU1 pipeline REJECTS whole triangles that cross the near plane
        // or blow the guard band (M4) -- it never clips them. Small triangles
        // make that invisible; an authored level's 40-unit floor quad makes
        // it a hole that swallows the foreground (M12.5). Subdividing at
        // export until no edge exceeds this WORLD-space length keeps the
        // holes at most one small triangle deep.
        //
        // How deep that is: a rejected floor triangle reaches up to one
        // edge length IN FRONT of the camera, and the floor becomes visible
        // at (camera height / tan(pitch + half the vertical FOV)) ahead.
        // For the usual third-person camera -- 2 units above the floor, 60
        // degree FOV -- that visible line sits 1-2 units ahead over the
        // whole pitch range, so 6-unit edges left a hole in the bottom of
        // every frame (the demo's orbit camera, verify-log). Below ~1.5 the
        // hole stays under the screen edge; the rule of thumb for a scene's
        // author is "edge length below the camera's height above nearby
        // surfaces". True near-plane clipping is the recorded follow-up
        // (ADR-003); until then the cost is vertices, paid only by meshes
        // with big triangles.
        public const float MaxTriangleEdgeWorld = 1.5f;
        // One MESH section holds at most this many triangles: 62 batches of
        // 26 in the worst layout, inside the runtime's kMaxBatchesPerMesh
        // of 64. A subdivided mesh over it is CHUNKED into several sections
        // (extra chunks ride on synthetic child entities) -- stopping the
        // subdivision early instead left giant triangles that flickered in
        // and out with camera rotation as the guard band rejected them.
        private const int MaxTrisPerChunk = 1600;
        // Explosion valve: 16 chunks (~25K triangles) per (mesh, submesh).
        private const int MaxChunks = 16;

        private struct Vert
        {
            public Vector3 P, N;
            public Vector2 T;
            public Color32 C;
        }

        // Returns ONE OR MORE MESH section payloads (see MaxTrisPerChunk).
        // 'submesh' selects one submesh's triangles; -1 exports them all
        // (single-material meshes and the legacy callers).
        public static System.Collections.Generic.List<byte[]> Export(
            Mesh mesh, uint kind, uint materialIndex, Color32 fallbackColour,
            float maxUserScale = 1f, int submesh = -1,
            Vector3? userScaleAxes = null)
        {
            Vector3[] positions = mesh.vertices;
            Vector3[] normals = mesh.normals;
            Vector2[] uvs = mesh.uv;
            Color32[] colours = mesh.colors32;
            int[] indices = submesh >= 0 && submesh < mesh.subMeshCount
                                ? mesh.GetTriangles(submesh)
                                : mesh.triangles;

            // Deindex, then split large triangles. The threshold is world
            // units; meshes are object space, so divide by the largest scale
            // any entity applies to this mesh.
            var verts = new System.Collections.Generic.List<Vert>(indices.Length);
            for (int i = 0; i < indices.Length; i++)
            {
                int src = indices[i];
                verts.Add(new Vert
                {
                    P = positions[src],
                    N = normals.Length > src ? normals[src] : Vector3.up,
                    T = uvs.Length > src ? uvs[src] : Vector2.zero,
                    C = colours.Length > src ? colours[src] : fallbackColour,
                });
            }
            // Edges are measured in WORLD units: object-space lengths scaled
            // per axis by the largest user of the mesh. The single-scalar
            // form (largest axis on every axis) is kept for callers without
            // a scale vector; it over-splits anything flat -- a 17 x 1 x 15
            // floor slab's 1-unit sides were split as if they were 17 wide.
            Vector3 axes = userScaleAxes ??
                           new Vector3(maxUserScale, maxUserScale, maxUserScale);
            axes = new Vector3(Mathf.Max(Mathf.Abs(axes.x), 0.0001f),
                               Mathf.Max(Mathf.Abs(axes.y), 0.0001f),
                               Mathf.Max(Mathf.Abs(axes.z), 0.0001f));
            verts = Subdivide(verts, MaxTriangleEdgeWorld, axes);

            int totalTris = verts.Count / 3;
            if (totalTris > MaxTrisPerChunk * MaxChunks)
            {
                Debug.LogWarning(
                    $"[PS2] mesh '{mesh.name}' subdivides to {totalTris} " +
                    $"triangles; only the first {MaxTrisPerChunk * MaxChunks} " +
                    "are exported. Simplify the mesh or shrink it.");
            }

            // An empty submesh yields NO sections (the runtime refuses a
            // zero-batch mesh); the caller simply records nothing for it.
            var sections = new System.Collections.Generic.List<byte[]>();
            for (int chunk = 0; chunk * MaxTrisPerChunk < totalTris &&
                                chunk < MaxChunks; chunk++)
            {
                int firstTri = chunk * MaxTrisPerChunk;
                int tris = Mathf.Min(MaxTrisPerChunk, totalTris - firstTri);
                sections.Add(BuildSection(mesh, verts, firstTri * 3, tris * 3,
                                          kind, materialIndex, fallbackColour));
            }
            return sections;
        }

        private static byte[] BuildSection(
            Mesh mesh, System.Collections.Generic.List<Vert> verts,
            int firstVert, int vertCount, uint kind, uint materialIndex,
            Color32 fallbackColour)
        {
            int triVerts = vertCount;
            int maxPerBatch = UsesLitLayout(kind) ? MaxLitVertsPerBatch
                                                  : UsesTexLayout(kind)
                                                      ? MaxTexVertsPerBatch
                                                      : MaxUnlitVertsPerBatch;
            int qwordsPerVert = UsesLitLayout(kind) || UsesTexLayout(kind) ? 3 : 2;
            int batchCount = (triVerts + maxPerBatch - 1) / maxPerBatch;

            // Bounding sphere of THIS CHUNK's vertices, not the whole mesh:
            // chunks of a big backdrop cull individually.
            Vector3 mn = Vector3.zero, mx = Vector3.zero;
            for (int i = 0; i < vertCount; i++)
            {
                Vector3 p = verts[firstVert + i].P;
                if (i == 0) { mn = p; mx = p; }
                else
                {
                    mn = Vector3.Min(mn, p);
                    mx = Vector3.Max(mx, p);
                }
            }
            Vector3 centre = (mn + mx) * 0.5f;
            float radius = 0f;
            for (int i = 0; i < vertCount; i++)
            {
                float d = (verts[firstVert + i].P - centre).magnitude;
                if (d > radius) radius = d;
            }

            var header = new ByteBuffer();
            header.U32((uint)batchCount);
            header.U32(materialIndex);
            header.F32(centre.x);
            header.F32(centre.y);
            header.F32(centre.z);
            header.F32(radius);

            // Descs are fixed-size; blob offsets are computable up front.
            // Layout: 24-byte header, batchCount*16 descs, pad to 16, blobs.
            int descsBytes = batchCount * 16;
            int blobsStart = Align16(24 + descsBytes);

            var descs = new ByteBuffer();
            var blobs = new ByteBuffer();
            int blobCursorQw = blobsStart / 16;

            for (int batch = 0; batch < batchCount; batch++)
            {
                int first = batch * maxPerBatch;
                int n = Math.Min(maxPerBatch, triVerts - first);
                // Never split mid-triangle: maxPerBatch is a multiple of 3.
                int vertQw = n * qwordsPerVert;

                descs.U32((uint)blobCursorQw);
                descs.U32((uint)vertQw);
                descs.U32((uint)n);
                descs.U32(UsesLitLayout(kind) ? 18u : 10u);

                // Blob: tag, count, vertices.
                WriteGifTag(blobs, (uint)n, kind);
                blobs.U32((uint)n);
                blobs.U32(0);
                blobs.U64(0);

                for (int i = 0; i < n; i++)
                {
                    Vert v = verts[firstVert + first + i];
                    blobs.F32(v.P.x);
                    blobs.F32(v.P.y);
                    blobs.F32(v.P.z);
                    blobs.F32(1.0f);

                    if (UsesTexLayout(kind))
                    {
                        // GS texture space has V growing DOWN; Unity's grows up.
                        blobs.F32(v.T.x);
                        blobs.F32(1.0f - v.T.y);
                        blobs.F32(1.0f); // becomes Q after the 1/w multiply
                        blobs.F32(0.0f);
                    }
                    else if (UsesLitLayout(kind))
                    {
                        blobs.F32(v.N.x);
                        blobs.F32(v.N.y);
                        blobs.F32(v.N.z);
                        blobs.F32(0.0f);
                    }

                    Color32 c = v.C;
                    // GS modulate treats 0x80 as 1.0, so TEXTURED vertex
                    // colours live in the 0..128 range: a white vertex must
                    // be 128, or every texel is DOUBLED and anything brighter
                    // than mid-grey clips to flat white -- which is exactly
                    // what bright concrete and parking-lot sheets did, while
                    // dark asphalt hid it since M5 (verify-log M12.5).
                    // Untextured layouts keep 0..255: there the vertex colour
                    // IS the final colour.
                    float cscale = UsesTexLayout(kind) ? 128.0f / 255.0f : 1.0f;
                    blobs.F32(c.r * cscale);
                    blobs.F32(c.g * cscale);
                    blobs.F32(c.b * cscale);
                    // PS2 alpha range: 0x80 is opaque. Blend kinds carry the
                    // material/vertex alpha; opaque kinds pin fully opaque.
                    blobs.F32(UsesBlend(kind) ? c.a * 128.0f / 255.0f : 128.0f);
                }

                blobCursorQw += 2 + vertQw;
            }

            var section = new ByteBuffer();
            section.Bytes(header.ToArray());
            section.Bytes(descs.ToArray());
            section.PadTo(16);
            section.Bytes(blobs.ToArray());
            return section.ToArray();
        }

        // Longest-edge midpoint split until every edge is under maxEdge
        // (object units). Deliberately edge-of-triangle local -- no shared
        // topology -- because the list is already deindexed; a T-junction
        // between neighbours splits identically on both sides only when the
        // shared edge is the one split, which longest-edge selection does
        // not guarantee. In practice kit geometry is planar quads and the
        // seams land on interpolated values of the SAME plane, so nothing
        // cracks visually; lighting is per-vertex either way.
        private static System.Collections.Generic.List<Vert> Subdivide(
            System.Collections.Generic.List<Vert> tris, float maxEdge,
            Vector3 axes)
        {
            float maxSq = maxEdge * maxEdge;
            var work = new System.Collections.Generic.Stack<(Vert, Vert, Vert)>();
            for (int i = tris.Count - 3; i >= 0; i -= 3)
            {
                work.Push((tris[i], tris[i + 1], tris[i + 2]));
            }
            var outv = new System.Collections.Generic.List<Vert>(tris.Count);
            while (work.Count > 0)
            {
                (Vert a, Vert b, Vert c) = work.Pop();
                float ab = Vector3.Scale(a.P - b.P, axes).sqrMagnitude;
                float bc = Vector3.Scale(b.P - c.P, axes).sqrMagnitude;
                float ca = Vector3.Scale(c.P - a.P, axes).sqrMagnitude;
                float longest = Mathf.Max(ab, Mathf.Max(bc, ca));
                bool budget = outv.Count / 3 + work.Count + 2 <=
                              MaxTrisPerChunk * MaxChunks;
                if (longest <= maxSq || !budget)
                {
                    outv.Add(a);
                    outv.Add(b);
                    outv.Add(c);
                    continue;
                }
                if (longest == ab)
                {
                    Vert m = Mid(a, b);
                    work.Push((a, m, c));
                    work.Push((m, b, c));
                }
                else if (longest == bc)
                {
                    Vert m = Mid(b, c);
                    work.Push((a, b, m));
                    work.Push((a, m, c));
                }
                else
                {
                    Vert m = Mid(c, a);
                    work.Push((a, b, m));
                    work.Push((m, b, c));
                }
            }
            return outv;
        }

        private static Vert Mid(Vert a, Vert b)
        {
            return new Vert
            {
                P = (a.P + b.P) * 0.5f,
                N = (a.N + b.N).normalized,
                T = (a.T + b.T) * 0.5f,
                C = Color32.Lerp(a.C, b.C, 0.5f),
            };
        }

        // GIF tag identical to the runtime's GsPacket.begin_packed output.
        private static void WriteGifTag(ByteBuffer b, uint nloop, uint kind)
        {
            uint nreg = UsesTexLayout(kind) || kind == KindVertexLitFog ? 3u : 2u;
            ulong regs = kind == KindVertexLitFog
                             ? 0x5A1UL  // RGBAQ, FOG, XYZ2
                             : UsesTexLayout(kind)
                                 ? 0x512UL  // ST, RGBAQ, XYZ2
                                 : 0x51UL;  // RGBAQ, XYZ2
            ulong prim = GsPrim(kind);
            ulong lo = (nloop & 0x7FFFUL)
                       | (1UL << 15)                 // EOP
                       | (1UL << 46)                 // PRE: apply PRIM
                       | ((prim & 0x7FFUL) << 47)
                       | (0UL << 58)                 // FLG: PACKED
                       | ((ulong)(nreg & 0xFu) << 60);
            b.U64(lo);
            b.U64(regs);
        }

        private static ulong GsPrim(uint kind)
        {
            // prim=3 triangle | IIP gouraud | TME for textured; FST stays 0
            // (STQ perspective-correct texturing). ABE rides in the PRIM for
            // blend kinds (M8 task 5): the blend equation itself is the
            // material's ALPHA_1 register, set between chain kicks.
            ulong prim = 3UL | (1UL << 3);
            if (UsesTexLayout(kind))
            {
                prim |= 1UL << 4;
            }
            if (UsesBlend(kind))
            {
                prim |= 1UL << 6; // ABE
            }
            if (kind == KindVertexLitFog)
            {
                prim |= 1UL << 5; // FGE: blend toward FOGCOL by per-vertex F
            }
            return prim;
        }

        private static int Align16(int v) => (v + 15) & ~15;
    }
}
