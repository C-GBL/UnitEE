using System.Collections.Generic;
using UnityEngine;

namespace Ps2.Editor
{
    // Bakes one (font, size) pair using UNITY'S OWN rasteriser (M12.5):
    // RequestCharactersInTexture puts the glyphs in the font's dynamic
    // atlas exactly as the Editor would draw them, and this reads them
    // back out into a PS2 atlas -- white pixels, coverage in alpha, which
    // the ordinary texture exporter quantises losslessly (256 alpha
    // levels of one colour is exactly a 256-entry CLUT). The runtime
    // draws the result at 1:1 baked pixels: Unity's shapes, Unity's
    // sizes, Unity's spacing.
    //
    // Baked per (font, size): a Text whose fontSize changes at RUNTIME
    // cannot re-rasterise on a PS2 and keeps its baked glyphs
    // (deviation 30).
    internal static class P2bFontExporter
    {
        internal const int FirstChar = 32;
        internal const int LastChar = 126; // printable ASCII, 95 glyphs

        internal sealed class BakedFont
        {
            public byte[] Coverage;    // AtlasW x AtlasH, TOP-DOWN rows
            public int AtlasW, AtlasH;
            public float Ascent;       // px above the baseline
            public float LineHeight;   // px per line
            public GlyphRecord[] Glyphs = new GlyphRecord[LastChar - FirstChar + 1];
        }

        internal struct GlyphRecord
        {
            public int U, V, W, H;
            public int BearingX, BearingY;
            public int Advance;
        }

        internal static BakedFont Bake(Font font, int size,
                                       List<string> warnings)
        {
            var chars = new char[LastChar - FirstChar + 1];
            for (int i = 0; i < chars.Length; i++)
                chars[i] = (char)(FirstChar + i);
            string all = new string(chars);

            // One request for EVERYTHING, then read, then query: a second
            // request can rebuild the dynamic atlas and invalidate every
            // CharacterInfo fetched before it.
            font.RequestCharactersInTexture(all, size, FontStyle.Normal);
            var source = font.material != null
                             ? font.material.mainTexture as Texture2D
                             : null;
            if (source == null)
            {
                warnings.Add("font '" + font.name + "' has no readable atlas; " +
                             "its Text falls back to the builtin 8x8 font.");
                return null;
            }
            Color32[] srcPixels = ReadAtlas(source);
            if (srcPixels == null)
            {
                warnings.Add("font '" + font.name + "' atlas could not be " +
                             "read; falling back to the builtin 8x8 font.");
                return null;
            }
            int srcW = source.width, srcH = source.height;

            var baked = new BakedFont();

            // Shelf-pack into a fixed-width atlas; height grows to the next
            // power of two at the end. 1px gutters so CLAMP sampling never
            // pulls a neighbour in.
            const int atlasW = 256;
            int penX = 1, penY = 1, shelfH = 0;
            float maxTop = 0, maxBottom = 0;

            var placements = new List<(int glyph, CharacterInfo ci, int x, int y)>();
            for (int i = 0; i < chars.Length; i++)
            {
                if (!font.GetCharacterInfo(chars[i], out CharacterInfo ci, size))
                    continue;
                int gw = ci.glyphWidth, gh = ci.glyphHeight;
                if (ci.maxY > maxTop) maxTop = ci.maxY;
                if (-ci.minY > maxBottom) maxBottom = -ci.minY;
                if (gw <= 0 || gh <= 0)
                {
                    baked.Glyphs[i] = new GlyphRecord { Advance = ci.advance };
                    continue;
                }
                if (penX + gw + 1 > atlasW)
                {
                    penX = 1;
                    penY += shelfH + 1;
                    shelfH = 0;
                }
                placements.Add((i, ci, penX, penY));
                baked.Glyphs[i] = new GlyphRecord
                {
                    U = penX, V = penY, W = gw, H = gh,
                    BearingX = ci.minX, BearingY = ci.maxY,
                    Advance = ci.advance,
                };
                penX += gw + 1;
                if (gh > shelfH) shelfH = gh;
            }
            int atlasH = Mathf.NextPowerOfTwo(penY + shelfH + 1);
            if (atlasH > 256)
            {
                warnings.Add("font '" + font.name + "' at " + size + "px " +
                             "needs a " + atlasW + "x" + atlasH + " atlas; " +
                             "VRAM will be tight. Consider a smaller size.");
            }

            var coverage = new byte[atlasW * atlasH];
            int peak = 0;
            foreach (var (glyph, ci, gx, gy) in placements)
            {
                // Per-pixel UV interpolation between the four corners
                // handles the rotated/flipped placements Unity's dynamic
                // atlas produces. Coverage is the MAX channel: which channel
                // a font atlas carries it in depends on the backend (Alpha8
                // reads in .a, R8 in .r), and reading the wrong one shipped
                // a 27%-grey font (verify-log M12.5).
                int gw = ci.glyphWidth, gh = ci.glyphHeight;
                for (int y = 0; y < gh; y++)
                {
                    float ty = gh > 1 ? (y + 0.5f) / gh : 0.5f;
                    Vector2 rowL = Vector2.Lerp(ci.uvTopLeft, ci.uvBottomLeft, ty);
                    Vector2 rowR = Vector2.Lerp(ci.uvTopRight, ci.uvBottomRight, ty);
                    for (int x = 0; x < gw; x++)
                    {
                        float tx = gw > 1 ? (x + 0.5f) / gw : 0.5f;
                        Vector2 uv = Vector2.Lerp(rowL, rowR, tx);
                        int sx = Mathf.Clamp((int)(uv.x * srcW), 0, srcW - 1);
                        int sy = Mathf.Clamp((int)(uv.y * srcH), 0, srcH - 1);
                        Color32 s = srcPixels[sy * srcW + sx];
                        int c = Mathf.Max(Mathf.Max(s.r, s.g),
                                          Mathf.Max(s.b, s.a));
                        if (c > peak) peak = c;
                        coverage[(gy + y) * atlasW + (gx + x)] = (byte)c;
                    }
                }
            }
            // Normalise so full coverage is full alpha: blit paths through
            // RenderTextures can attenuate (colour-space conversions among
            // them), and a font whose peak is 68/255 draws as grey text.
            // Every real font has fully-opaque cores, so the peak IS 100%.
            if (peak > 0 && peak < 255)
            {
                for (int i = 0; i < coverage.Length; i++)
                    coverage[i] = (byte)(coverage[i] * 255 / peak);
            }

            baked.Coverage = coverage;
            baked.AtlasW = atlasW;
            baked.AtlasH = atlasH;

            baked.Ascent = maxTop;
            // Ascent + descent + 2px leading approximates Unity's default
            // single spacing; multi-line leading is cosmetic, single lines
            // are exact.
            baked.LineHeight = maxTop + maxBottom + 2f;
            return baked;
        }

        // The FONT section payload: 32-byte header + 12 bytes per glyph
        // (docs/formats/p2b-container.md). The caller supplies the TEX
        // index it registered the atlas under.
        internal static byte[] BuildSection(BakedFont baked, int textureIndex)
        {
            var b = new ByteBuffer();
            b.U32((uint)textureIndex);
            b.U32((uint)baked.Glyphs.Length);
            b.F32(baked.Ascent);
            b.F32(baked.LineHeight);
            b.U32(FirstChar);
            b.U32(0);
            b.U32(0);
            b.U32(0);
            foreach (GlyphRecord g in baked.Glyphs)
            {
                b.U16((ushort)g.U);
                b.U16((ushort)g.V);
                b.U8((byte)Mathf.Clamp(g.W, 0, 255));
                b.U8((byte)Mathf.Clamp(g.H, 0, 255));
                b.U8((byte)(sbyte)Mathf.Clamp(g.BearingX, -128, 127));
                b.U8((byte)(sbyte)Mathf.Clamp(g.BearingY, -128, 127));
                b.U16((ushort)Mathf.Clamp(g.Advance << 4, 0, ushort.MaxValue));
                b.U16(0);
            }
            return b.ToArray();
        }

        // The atlas TEX section, encoded DIRECTLY: the palette of a font
        // atlas is known by construction -- 256 levels of white -- so the
        // index IS the coverage byte and the CLUT is a ramp. The general
        // median-cut quantiser is built for art and collapsed the alpha
        // ramp to eight levels (the banded text of verify-log M12.5);
        // a known palette needs no quantiser at all.
        internal static byte[] BuildAtlasSection(BakedFont baked)
        {
            var entries = new uint[256];
            for (uint i = 0; i < 256; i++)
            {
                uint a = (i * 128 + 127) / 255; // PS2 alpha range
                entries[i] = 0xFFFFFFu | (a << 24);
            }
            uint[] csm1 = P2bTextureExporter.Csm1Reorder(entries);

            var b = new ByteBuffer();
            b.U32((uint)baked.AtlasW);
            b.U32((uint)baked.AtlasH);
            b.U32(19); // PSMT8, the same constant Export writes
            b.U32(256);
            foreach (uint e in csm1)
                b.U32(e);
            // Coverage rows are already TOP-DOWN, which is the on-disc
            // orientation (Export flips Unity's bottom-up arrays to get
            // here; this data never was bottom-up).
            b.Bytes(baked.Coverage);
            return b.ToArray();
        }

        // The dynamic font atlas is not CPU-readable; round-trip it
        // through a RenderTexture, the same move the texture exporter
        // makes for unreadable art (M12.5 task 1).
        private static Color32[] ReadAtlas(Texture2D source)
        {
            var rt = RenderTexture.GetTemporary(source.width, source.height, 0,
                                                RenderTextureFormat.ARGB32);
            RenderTexture previous = RenderTexture.active;
            Graphics.Blit(source, rt);
            RenderTexture.active = rt;
            var readable = new Texture2D(source.width, source.height,
                                         TextureFormat.RGBA32, false);
            readable.ReadPixels(new Rect(0, 0, source.width, source.height),
                                0, 0);
            readable.Apply();
            RenderTexture.active = previous;
            RenderTexture.ReleaseTemporary(rt);
            Color32[] pixels = readable.GetPixels32();
            Object.DestroyImmediate(readable);
            return pixels;
        }
    }
}
