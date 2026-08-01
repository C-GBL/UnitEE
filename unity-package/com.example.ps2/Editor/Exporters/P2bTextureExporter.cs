// Texture2D -> TEX section (plan section 9 M5 task 3 + M3 task 1 offline
// half): median-cut quantisation to PSMT8, PS2 alpha rescale, CSM1 palette
// reordering. Indices stay in RASTER order -- the GS transfer engine swizzles
// during upload (verify-log 2026-08-01), so the exporter must NOT.
using System;
using System.Collections.Generic;
using UnityEngine;

namespace Ps2.Editor
{
    internal static class P2bTextureExporter
    {
        public const uint FormatPsmt8 = 0x13;

        public static byte[] Export(Texture2D texture)
        {
            Color32[] pixels = texture.GetPixels32();
            int w = texture.width;
            int h = texture.height;

            byte[] indices;
            Color32[] palette = MedianCut(pixels, 256, out indices);

            // GS raster origin is top-left; GetPixels32 is bottom-up. Flip.
            var flipped = new byte[indices.Length];
            for (int y = 0; y < h; y++)
            {
                Array.Copy(indices, (h - 1 - y) * w, flipped, y * w, w);
            }

            // Palette: PS2 alpha (0..128), then CSM1 storage order.
            var entries = new uint[256];
            for (int i = 0; i < palette.Length; i++)
            {
                Color32 c = palette[i];
                uint a = (uint)((c.a * 128 + 127) / 255);
                entries[i] = c.r | ((uint)c.g << 8) | ((uint)c.b << 16) | (a << 24);
            }
            uint[] csm1 = Csm1Reorder(entries);

            var b = new ByteBuffer();
            b.U32((uint)w);
            b.U32((uint)h);
            b.U32(FormatPsmt8);
            b.U32(256);
            foreach (uint e in csm1)
            {
                b.U32(e);
            }
            b.Bytes(flipped);
            return b.ToArray();
        }

        // Same permutation as runtime clut_csm1_reorder: within each group of
        // 8 blocks of 8 entries, blocks 1<->2 and 5<->6 swap.
        public static uint[] Csm1Reorder(uint[] src)
        {
            var dst = new uint[256];
            for (int i = 0; i < 256; i++)
            {
                int block = (i >> 3) & 7;
                int rest = i & ~0x38;
                int swapped = block == 1 ? 2 : block == 2 ? 1
                              : block == 5 ? 6 : block == 6 ? 5 : block;
                dst[rest | (swapped << 3)] = src[i];
            }
            return dst;
        }

        // Straightforward median-cut: split the box with the largest channel
        // range at its median until we have 'colours' boxes, average each.
        private static Color32[] MedianCut(Color32[] pixels, int colours,
                                           out byte[] indices)
        {
            var boxes = new List<List<int>> { new List<int>(pixels.Length) };
            for (int i = 0; i < pixels.Length; i++)
            {
                boxes[0].Add(i);
            }

            while (boxes.Count < colours)
            {
                int best = -1;
                int bestRange = -1;
                int bestChannel = 0;
                for (int bi = 0; bi < boxes.Count; bi++)
                {
                    if (boxes[bi].Count < 2)
                    {
                        continue;
                    }
                    for (int ch = 0; ch < 3; ch++)
                    {
                        int lo = 255, hi = 0;
                        foreach (int pi in boxes[bi])
                        {
                            int v = Channel(pixels[pi], ch);
                            if (v < lo) lo = v;
                            if (v > hi) hi = v;
                        }
                        if (hi - lo > bestRange)
                        {
                            bestRange = hi - lo;
                            best = bi;
                            bestChannel = ch;
                        }
                    }
                }
                if (best < 0 || bestRange == 0)
                {
                    break; // fewer distinct colours than requested
                }
                var box = boxes[best];
                int channel = bestChannel;
                box.Sort((a, bIdx) =>
                    Channel(pixels[a], channel) - Channel(pixels[bIdx], channel));
                int mid = box.Count / 2;
                var upper = box.GetRange(mid, box.Count - mid);
                box.RemoveRange(mid, box.Count - mid);
                boxes.Add(upper);
            }

            var palette = new Color32[256];
            var paletteOf = new byte[pixels.Length];
            for (int bi = 0; bi < boxes.Count; bi++)
            {
                long r = 0, g = 0, bl = 0, a = 0;
                foreach (int pi in boxes[bi])
                {
                    r += pixels[pi].r;
                    g += pixels[pi].g;
                    bl += pixels[pi].b;
                    a += pixels[pi].a;
                }
                int n = Math.Max(1, boxes[bi].Count);
                palette[bi] = new Color32((byte)(r / n), (byte)(g / n),
                                          (byte)(bl / n), (byte)(a / n));
                foreach (int pi in boxes[bi])
                {
                    paletteOf[pi] = (byte)bi;
                }
            }
            indices = paletteOf;
            return palette;
        }

        private static int Channel(Color32 c, int ch) =>
            ch == 0 ? c.r : ch == 1 ? c.g : c.b;
    }
}
