using UnityEngine;

namespace Ps2.Editor
{
    /// <summary>
    /// Encodes AudioClips to SPU2-native 4-bit ADPCM (VAG) -- the "ADPCM encoder"
    /// converter of the plan section 6 diagram. Plan section 3.5: SPU2 has 48
    /// hardware voices and 2 MB of sound RAM; music streams from disc through a
    /// SIF-fed ring buffer while SFX stay resident.
    /// </summary>
    public static class PS2AudioExporter
    {
        /// <summary>Exports one clip as ADPCM (resident SFX or stream) into &lt;outDir&gt;.</summary>
        public static void ExportClip(AudioClip clip, string outDir)
        {
            throw new PS2BuildException(
                "PS2AudioExporter not implemented; blocked on the p2b container spec (plan section 10, " +
                "missing). TODO(spec missing: section 10). Clip: '" +
                (clip != null ? clip.name : "<null>") + "', outDir: '" + outDir + "'.");
        }
    }
}
