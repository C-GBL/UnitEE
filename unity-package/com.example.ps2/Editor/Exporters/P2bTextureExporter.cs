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

        // maxSize is the profile's Texture Max Size. Passing 0 keeps the
        // source resolution.
        //
        // This used to be validated and never applied: a build could warn that
        // a 1024x1024 texture was over the limit and then put all 1 MB of it
        // on the disc, where it did not fit in the ~1.4 MB of VRAM left after
        // the framebuffers and the upload failed at boot (verify-log M12.5).
        public static byte[] Export(Texture2D texture, int maxSize = 0)
        {
            int w = texture.width;
            int h = texture.height;
            int tw = w, th = h;
            if (maxSize > 0 && (w > maxSize || h > maxSize))
            {
                // Preserve aspect, then snap to powers of two: the GS
                // addresses textures by log2 dimensions, so a non-power-of-two
                // is not a slower texture, it is an unrepresentable one.
                float scale = Mathf.Min((float)maxSize / w, (float)maxSize / h);
                tw = Pow2AtMost(Mathf.Max(1, Mathf.RoundToInt(w * scale)), maxSize);
                th = Pow2AtMost(Mathf.Max(1, Mathf.RoundToInt(h * scale)), maxSize);
            }

            Color32[] pixels = (tw == w && th == h) ? ReadPixels(texture)
                                                    : ReadScaled(texture, tw, th);
            w = tw;
            h = th;

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

        // Texture pixels, whether or not the asset is marked Read/Write
        // Enabled.
        //
        // Almost no real project enables it -- it keeps a second copy of every
        // texture in CPU memory, so Unity defaults it off and imported art
        // (Unity-chan included) ships with it off. GetPixels32 fails on those,
        // and on block-compressed formats besides, which quietly limited this
        // exporter to assets that had been prepared for it by hand.
        internal static Color32[] ReadPixels(Texture2D texture)
        {
            // Prefer the direct read where the asset allows it: it is exact,
            // it needs no GPU, and it keeps every already-golden texture
            // byte-identical.
            if (texture.isReadable)
            {
                return texture.GetPixels32();
            }
            VerifyBlitRoundTrip();
            return ReadViaBlit(texture);
        }

        // The largest power of two that is <= v and <= cap.
        private static int Pow2AtMost(int v, int cap)
        {
            int p = 1;
            while (p * 2 <= v && p * 2 <= cap)
            {
                p *= 2;
            }
            return p;
        }

        // Downscale on the GPU. Blit already resamples, so a smaller target is
        // a filtered resize for free -- and it works on the non-readable,
        // compressed textures that imported art actually consists of.
        internal static Color32[] ReadScaled(Texture2D texture, int w, int h)
        {
            VerifyBlitRoundTrip();
            return ReadViaBlit(texture, w, h);
        }

        // Copy through the GPU, which can read any format, and read that back.
        internal static Color32[] ReadViaBlit(Texture texture)
        {
            return ReadViaBlit(texture, texture.width, texture.height);
        }

        internal static Color32[] ReadViaBlit(Texture texture, int w, int h)
        {
            // sRGB, not Linear. In a linear-colour-space project the sampler
            // converts sRGB->linear when it reads an sRGB texture, and an sRGB
            // target converts back on write, so the two cancel and we recover
            // the bytes the artist authored -- which is what the GS wants,
            // having no linear pipeline of its own to undo. In a gamma project
            // Unity performs neither conversion and this is a plain copy.
            RenderTexture rt = RenderTexture.GetTemporary(
                w, h, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.sRGB);
            RenderTexture previous = RenderTexture.active;
            Texture2D readable = null;
            try
            {
                Graphics.Blit(texture, rt);
                RenderTexture.active = rt;
                readable = new Texture2D(w, h, TextureFormat.RGBA32, false, false);
                readable.ReadPixels(new Rect(0, 0, w, h), 0, 0);
                readable.Apply(false, false);
                return readable.GetPixels32();
            }
            finally
            {
                RenderTexture.active = previous;
                RenderTexture.ReleaseTemporary(rt);
                if (readable != null)
                {
                    UnityEngine.Object.DestroyImmediate(readable);
                }
            }
        }

        // Blit-and-read-back is what every asset tool does, but two parts of it
        // are decided by the graphics API rather than by this code: whether
        // Blit flips vertically (its flip and ReadPixels' are meant to cancel)
        // and whether the sRGB round trip is faithful. Get either wrong and the
        // build ships textures that are upside down or subtly off in colour --
        // a picture that still looks plausible, which is the worst failure this
        // pipeline can produce. It also silently produces nothing at all under
        // -nographics, where there is no GPU to blit with.
        //
        // So assert it once per domain reload, against a texture whose pixels
        // are known and deliberately not symmetric in either axis.
        private static bool s_BlitVerified;

        private static void VerifyBlitRoundTrip()
        {
            if (s_BlitVerified)
            {
                return;
            }
            s_BlitVerified = true;

            const int n = 8;
            var expected = new Color32[n * n];
            for (int y = 0; y < n; y++)
            {
                for (int x = 0; x < n; x++)
                {
                    // Distinct along x, along y, and between the two, so
                    // neither a flip nor a transpose can pass for a copy.
                    expected[y * n + x] = new Color32(
                        (byte)(x * 32), (byte)(y * 8),
                        (byte)(x == 0 && y == 0 ? 255 : 0), 255);
                }
            }

            var probe = new Texture2D(n, n, TextureFormat.RGBA32, false, false);
            try
            {
                probe.SetPixels32(expected);
                probe.Apply(false, false);
                Color32[] actual = ReadViaBlit(probe);

                for (int i = 0; i < expected.Length; i++)
                {
                    // One LSB of tolerance: the sRGB->linear->sRGB round trip
                    // happens in float and can land a step either side. That is
                    // far below the median-cut quantiser's resolution, while a
                    // flip or a transpose is off by whole channels.
                    if (Diff(actual[i].r, expected[i].r) > 1 ||
                        Diff(actual[i].g, expected[i].g) > 1 ||
                        Diff(actual[i].b, expected[i].b) > 1 ||
                        Diff(actual[i].a, expected[i].a) > 1)
                    {
                        throw new InvalidOperationException(
                            "texture read-back does not round trip on this " +
                            "graphics API (pixel " + i + " of the probe: wrote " +
                            expected[i] + ", read back " + actual[i] + "). " +
                            "Textures without Read/Write Enabled cannot be " +
                            "exported correctly until this is resolved; is the " +
                            "Editor running with -nographics?");
                    }
                }
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(probe);
            }
        }

        private static int Diff(byte a, byte b) => a > b ? a - b : b - a;

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
