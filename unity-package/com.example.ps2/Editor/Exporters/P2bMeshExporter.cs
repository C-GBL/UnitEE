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
        // holes at most one small triangle deep. 6 world units against the
        // usual 0.3-0.5 near plane keeps rejection artifacts under a few
        // pixels; the cost is vertices, paid only by meshes with big
        // triangles.
        public const float MaxTriangleEdgeWorld = 6f;
        // Safety valve: a degenerate scale or a giant terrain cannot explode
        // the container. Subdivision stops at this many triangles per mesh,
        // chosen to keep the worst layout (26 tris a batch) inside the
        // runtime's kMaxBatchesPerMesh of 64.
        private const int MaxSubdividedTris = 1600;

        private struct Vert
        {
            public Vector3 P, N;
            public Vector2 T;
            public Color32 C;
        }

        public static byte[] Export(Mesh mesh, uint kind, uint materialIndex,
                                    Color32 fallbackColour,
                                    float maxUserScale = 1f)
        {
            Vector3[] positions = mesh.vertices;
            Vector3[] normals = mesh.normals;
            Vector2[] uvs = mesh.uv;
            Color32[] colours = mesh.colors32;
            int[] indices = mesh.triangles; // all submeshes, deindexed below

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
            float maxEdge = MaxTriangleEdgeWorld /
                            Mathf.Max(maxUserScale, 0.0001f);
            verts = Subdivide(verts, maxEdge);

            int triVerts = verts.Count;
            int maxPerBatch = UsesLitLayout(kind) ? MaxLitVertsPerBatch
                                                  : UsesTexLayout(kind)
                                                      ? MaxTexVertsPerBatch
                                                      : MaxUnlitVertsPerBatch;
            int qwordsPerVert = UsesLitLayout(kind) || UsesTexLayout(kind) ? 3 : 2;
            int batchCount = (triVerts + maxPerBatch - 1) / maxPerBatch;
            // Matches the runtime's kMaxBatchesPerMesh. A mesh over it will
            // be REFUSED at load ("bad batch count"); say so while the mesh
            // still has a name and an owner who can act on it.
            const int maxBatchesPerMesh = 64;
            if (batchCount > maxBatchesPerMesh)
            {
                Debug.LogWarning(
                    $"[PS2] mesh '{mesh.name}' needs {batchCount} VU1 batches; " +
                    $"the runtime accepts {maxBatchesPerMesh} (~1,600 " +
                    "triangles). The scene will fail to load until this mesh " +
                    "is simplified or split.");
            }

            // Object-space bounding sphere from Unity's own bounds.
            Bounds b = mesh.bounds;
            float radius = b.extents.magnitude;

            var header = new ByteBuffer();
            header.U32((uint)batchCount);
            header.U32(materialIndex);
            header.F32(b.center.x);
            header.F32(b.center.y);
            header.F32(b.center.z);
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
                    Vert v = verts[first + i];
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
                    blobs.F32(c.r);
                    blobs.F32(c.g);
                    blobs.F32(c.b);
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
            System.Collections.Generic.List<Vert> tris, float maxEdge)
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
                float ab = (a.P - b.P).sqrMagnitude;
                float bc = (b.P - c.P).sqrMagnitude;
                float ca = (c.P - a.P).sqrMagnitude;
                float longest = Mathf.Max(ab, Mathf.Max(bc, ca));
                bool budget = outv.Count / 3 + work.Count + 2 <= MaxSubdividedTris;
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
