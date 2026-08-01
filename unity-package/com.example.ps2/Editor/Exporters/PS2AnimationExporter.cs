using UnityEngine;

namespace Ps2.Editor
{
    /// <summary>
    /// Samples AnimationClips in the Editor and quantises them for the ps2ur anim
    /// system (the "anim quantiser" converter of the plan section 6 diagram).
    /// Supported playback model per plan section 7.1: clip playback with crossfade,
    /// additive blending, root motion, and a simplified state-machine Animator.
    /// </summary>
    public static class PS2AnimationExporter
    {
        /// <summary>Exports one clip to a quantised track blob inside &lt;outDir&gt;.</summary>
        public static void ExportClip(AnimationClip clip, string outDir)
        {
            throw new PS2BuildException(
                "PS2AnimationExporter not implemented; blocked on the p2b container spec (plan section 10, " +
                "missing). TODO(spec missing: section 10). Clip: '" +
                (clip != null ? clip.name : "<null>") + "', outDir: '" + outDir + "'.");
        }
    }
}
