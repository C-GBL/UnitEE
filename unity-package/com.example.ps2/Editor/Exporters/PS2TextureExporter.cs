using UnityEngine;

namespace Ps2.Editor
{
    /// <summary>
    /// Palettises and swizzles textures offline for the GS.
    ///
    /// Facts this exporter will implement, recorded from plan section 3.3:
    ///
    /// - GS VRAM is 4 MB total for framebuffers, Z and textures, organised in pages
    ///   (8 KB) and blocks (256 bytes) with format-specific swizzle patterns.
    ///   Textures MUST be swizzled offline by this exporter; swizzling at runtime
    ///   wastes EE cycles.
    /// - Preferred output formats are the indexed ones: PSMT8 (8-bit indexed + CLUT)
    ///   and PSMT4 (4-bit indexed + CLUT). A 256x256 PSMT8 texture is 64 KB + 1 KB
    ///   CLUT; the same texture in PSMCT32 is 256 KB (only ~9 fit in the whole
    ///   machine). PSMCT32/PSMCT24/PSMCT16 remain available for special cases.
    /// - CSM1 CLUT quirk: for 8-bit indexed textures the 256-entry palette is stored
    ///   with its 32-entry blocks reordered -- blocks 1 and 2, and blocks 5 and 6, of
    ///   each group of 8 are swapped. The exporter must emit palettes pre-swapped.
    ///   Getting this wrong produces textures with correct shapes and scrambled
    ///   colours -- a deliberate early test case (see samples/01-spinning-cube).
    /// - The per-profile texture budget (PS2BuildProfile.textureBudgetKb) reflects
    ///   the section 3.3 baseline: ~1.2 MB of VRAM left for textures after the
    ///   512x448i PSMCT32 double-buffered colour + PSMZ24 Z framebuffer layout.
    /// </summary>
    public static class PS2TextureExporter
    {
        /// <summary>Exports one texture (quantised, swizzled, CLUT pre-swapped) into &lt;outDir&gt;.</summary>
        public static void ExportTexture(Texture2D texture, string outDir)
        {
            throw new PS2BuildException(
                "PS2TextureExporter not implemented; blocked on the p2b container spec (plan section 10, " +
                "missing). TODO(spec missing: section 10). Texture: '" +
                (texture != null ? texture.name : "<null>") + "', outDir: '" + outDir + "'.");
        }
    }
}
