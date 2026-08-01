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

        // Material kinds, matching the runtime's p2b_scene.h.
        public const uint KindUnlit = 0;
        public const uint KindUnlitTextured = 1;
        public const uint KindVertexLit = 2;

        public static byte[] Export(Mesh mesh, uint kind, uint materialIndex,
                                    Color32 fallbackColour)
        {
            Vector3[] positions = mesh.vertices;
            Vector3[] normals = mesh.normals;
            Vector2[] uvs = mesh.uv;
            Color32[] colours = mesh.colors32;
            int[] indices = mesh.triangles; // all submeshes, deindexed below

            int triVerts = indices.Length;
            int maxPerBatch = kind == KindUnlit ? MaxUnlitVertsPerBatch
                                                : kind == KindUnlitTextured
                                                    ? MaxTexVertsPerBatch
                                                    : MaxLitVertsPerBatch;
            int qwordsPerVert = kind == KindUnlit ? 2 : 3;
            int batchCount = (triVerts + maxPerBatch - 1) / maxPerBatch;

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
                descs.U32(kind == KindVertexLit ? 18u : 10u);

                // Blob: tag, count, vertices.
                WriteGifTag(blobs, (uint)n, kind);
                blobs.U32((uint)n);
                blobs.U32(0);
                blobs.U64(0);

                for (int i = 0; i < n; i++)
                {
                    int src = indices[first + i];
                    Vector3 p = positions[src];
                    blobs.F32(p.x);
                    blobs.F32(p.y);
                    blobs.F32(p.z);
                    blobs.F32(1.0f);

                    if (kind == KindUnlitTextured)
                    {
                        Vector2 uv = uvs.Length > src ? uvs[src] : Vector2.zero;
                        blobs.F32(uv.x);
                        // GS texture space has V growing DOWN; Unity's grows up.
                        blobs.F32(1.0f - uv.y);
                        blobs.F32(1.0f); // becomes Q after the 1/w multiply
                        blobs.F32(0.0f);
                    }
                    else if (kind == KindVertexLit)
                    {
                        Vector3 nrm = normals.Length > src ? normals[src]
                                                           : Vector3.up;
                        blobs.F32(nrm.x);
                        blobs.F32(nrm.y);
                        blobs.F32(nrm.z);
                        blobs.F32(0.0f);
                    }

                    Color32 c = colours.Length > src ? colours[src] : fallbackColour;
                    blobs.F32(c.r);
                    blobs.F32(c.g);
                    blobs.F32(c.b);
                    blobs.F32(128.0f); // opaque in PS2 alpha range
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

        // GIF tag identical to the runtime's GsPacket.begin_packed output.
        private static void WriteGifTag(ByteBuffer b, uint nloop, uint kind)
        {
            uint nreg = kind == KindUnlitTextured ? 3u : 2u;
            ulong regs = kind == KindUnlitTextured
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
            // (STQ perspective-correct texturing).
            ulong prim = 3UL | (1UL << 3);
            if (kind == KindUnlitTextured)
            {
                prim |= 1UL << 4;
            }
            return prim;
        }

        private static int Align16(int v) => (v + 15) & ~15;
    }
}
