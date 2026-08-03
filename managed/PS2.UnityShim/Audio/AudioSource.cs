using UnityEngine.Internal;

namespace UnityEngine
{
    // The managed face of an M10 mixer voice (M12.5 task 2).
    //
    // SUBSET DISCIPLINE (ADR-007): every member behaves as Unity's does,
    // with the deviations named in docs/supported-api.md:
    //
    //   - loop is read-only in effect: SPU2 loop points are baked into the
    //     ADPCM stream at export, so the value here is what the Editor
    //     authored. Assigning a DIFFERENT value logs an error naming the
    //     deviation instead of silently not looping (deviation 26).
    //   - isPlaying tracks the voice Play() started; one-shots are
    //     fire-and-forget and do not register (Unity counts them;
    //     deviation 27).
    //   - A sound can be dropped when all 24 voices are busy with
    //     higher-priority audio. Play() then leaves isPlaying false, which
    //     is exactly how the platform behaves (M10).
    //
    // spatialBlend is authored in the Editor and baked to a flag: > 0.5
    // means 3D (per-frame pan and linear attenuation against the listener,
    // natively), else 2D centred. Changing it at runtime is deviation 26's
    // territory too and is not exposed.
    public sealed class AudioSource : Behaviour
    {
        private int m_Voice;
        private float m_Volume = 1.0f;
        private bool m_Loop;
        private bool m_Spatial;
        private float m_MinDistance = 1.0f;
        private float m_MaxDistance = 30.0f;
        private int m_Priority = 128; // Unity scale: 0 most important
        private bool m_WarnedLoop;

        public AudioClip clip { get; set; }

        // Unity: 0..1, applies live to a playing source.
        public float volume
        {
            get => m_Volume;
            set
            {
                m_Volume = Mathf.Clamp01(value);
                if (m_Voice != 0)
                {
                    Native.ps2ur_audio_set_volume(m_Voice, m_Volume);
                }
            }
        }

        public bool loop
        {
            get => m_Loop;
            set
            {
                if (value != m_Loop && !m_WarnedLoop)
                {
                    m_WarnedLoop = true;
                    Debug.LogError(
                        "AudioSource.loop cannot change at runtime on PS2: " +
                        "loop points are baked into the clip at export. Set " +
                        "Loop on the source in the Editor. Deviation 26 in " +
                        "docs/supported-api.md.");
                }
            }
        }

        // Unity: 0 most important .. 256 least. Read by Play().
        public int priority
        {
            get => m_Priority;
            set => m_Priority = Mathf.Clamp(value, 0, 256);
        }

        public bool isPlaying =>
            m_Voice != 0 && Native.ps2ur_audio_is_playing(m_Voice) != 0;

        public void Play()
        {
            if (clip == null)
            {
                return; // exactly Unity: a source with no clip plays nothing
            }
            if (m_Voice != 0)
            {
                Native.ps2ur_audio_stop(m_Voice); // Play restarts (Unity)
            }
            m_Voice = Native.ps2ur_audio_play(
                gameObject != null ? gameObject.Handle : 0, clip.Index,
                m_Volume, NativePriority(), m_Spatial ? 1 : 0, m_MinDistance,
                m_MaxDistance);
        }

        public void Stop()
        {
            if (m_Voice != 0)
            {
                Native.ps2ur_audio_stop(m_Voice);
                m_Voice = 0;
            }
        }

        // Overlapping, never tracked, never stopped by Stop() -- Unity's
        // contract. The one-shot inherits the source's spatial setting.
        public void PlayOneShot(AudioClip oneShot)
        {
            PlayOneShot(oneShot, 1.0f);
        }

        public void PlayOneShot(AudioClip oneShot, float volumeScale)
        {
            if (oneShot == null)
            {
                return;
            }
            Native.ps2ur_audio_play(
                gameObject != null ? gameObject.Handle : 0, oneShot.Index,
                Mathf.Clamp01(m_Volume * volumeScale), NativePriority(),
                m_Spatial ? 1 : 0, m_MinDistance, m_MaxDistance);
        }

        // The mixer's scale is higher-wins; Unity's is lower-wins.
        private int NativePriority() => 256 - m_Priority;

        // Called by Runtime.CreateAudioSource with the Editor-authored state.
        internal void Configure(AudioClip bootClip, float bootVolume,
                                bool bootLoop, bool spatial, float minDistance,
                                float maxDistance, int nativePriority)
        {
            clip = bootClip;
            m_Volume = bootVolume;
            m_Loop = bootLoop;
            m_Spatial = spatial;
            m_MinDistance = minDistance;
            m_MaxDistance = maxDistance;
            m_Priority = Mathf.Clamp(256 - nativePriority, 0, 256);
        }
    }

    // Exactly one per scene, usually on the camera; its transform poses the
    // mixer's listener every frame (natively -- see bridge_audio.cpp). The
    // validator errors on a second one at build time, as Unity warns.
    public sealed class AudioListener : Behaviour
    {
    }
}
