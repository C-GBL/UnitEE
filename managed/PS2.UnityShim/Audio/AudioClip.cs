using UnityEngine.Internal;

namespace UnityEngine
{
    // A clip in SPU2 memory, loaded from the SND section at boot (plan
    // section 7.1 "Audio", M10 runtime, M12.5 task 2 reachability).
    //
    // SUBSET DISCIPLINE (ADR-007): what exists behaves as Unity's does.
    // length is the real duration from the encoded sample count and rate.
    // Absent on purpose: name (SND stores a hash, not the string -- carrying
    // every clip name to the console for a property nobody ships with would
    // be pure bytes), samples/channels/frequency (the SPU2 plays ADPCM; the
    // PCM view Unity exposes does not exist here), and GetData/SetData.
    public sealed class AudioClip : Object
    {
        internal readonly int Index;

        internal AudioClip(int index)
        {
            Index = index;
        }

        public float length => Native.ps2ur_audio_clip_seconds(Index);
    }
}
