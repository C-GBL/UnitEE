using UnityEngine;

namespace Ps2.Runtime
{
    /// <summary>
    /// The particle system the PS2 build target actually ships (M12.5 task 4,
    /// ADR-011): a fixed-capacity CPU simulation drawn as camera-facing quads.
    /// Deliberately not Unity's ParticleSystem -- this is the subset that maps
    /// to the hardware, authored as plain fields. The build validator errors
    /// on Unity's own ParticleSystem and names this as the replacement.
    ///
    /// The console-side class (in PS2.UnityShim) has the same full name, so
    /// scripts calling Play/Stop/Emit compile in both worlds. In Editor play
    /// mode this component is authoring data only; there is no preview yet.
    /// </summary>
    [AddComponentMenu("PS2/PS2 Particle System")]
    public sealed class PS2ParticleSystem : MonoBehaviour
    {
        public enum Shape
        {
            Sphere = 0,
            Cone = 1, // emits along the transform's +Z
            Box = 2,
        }

        [Header("Emission")]
        [Tooltip("Particles per second while playing.")]
        public float emissionRate = 10f;
        [Tooltip("Emitted all at once when the system starts playing.")]
        public int burstCount = 0;
        public bool looping = true;
        public bool playOnAwake = true;
        [Tooltip("Hard pool bound; the emitter cannot exceed it. Max 128.")]
        [Range(1, 128)] public int maxParticles = 128;

        [Header("Shape")]
        public Shape shape = Shape.Sphere;
        [Tooltip("Sphere radius / cone angle in degrees / box half-extent X.")]
        public float shapeA = 0.5f;
        [Tooltip("Cone base radius / box half-extent Y.")]
        public float shapeB = 0f;
        [Tooltip("Box half-extent Z.")]
        public float shapeC = 0f;

        [Header("Particles")]
        public float lifetime = 1f;
        public float speed = 1f;
        [Tooltip("Quad edge length in world units, at birth and death.")]
        public float sizeStart = 0.25f;
        public float sizeEnd = 0.25f;
        public Color colourStart = Color.white;
        public Color colourEnd = new Color(1f, 1f, 1f, 0f);
        [Tooltip("Multiplier of 9.81 downward.")]
        public float gravityModifier = 0f;

        [Header("Rendering")]
        [Tooltip("The particle texture; untextured quads without one.")]
        public Texture2D texture;
        [Tooltip("Additive blending (fire, sparks) instead of alpha (smoke).")]
        public bool additive = false;
        [Tooltip("Simulate in world space so particles trail a moving emitter.")]
        public bool worldSpace = true;

        // Console-side API stand-ins so scripts compile and run in Editor play
        // mode without crashing; there is no Editor simulation yet, and each
        // says so once rather than pretending.
        private static bool s_WarnedPreview;

        public void Play() => WarnPreview();
        public void Stop() => WarnPreview();
        public void Emit(int count) => WarnPreview();
        public bool isPlaying { get { WarnPreview(); return false; } }
        public int particleCount { get { WarnPreview(); return 0; } }

        private static void WarnPreview()
        {
            if (s_WarnedPreview || !Application.isPlaying) return;
            s_WarnedPreview = true;
            Debug.LogWarning(
                "PS2ParticleSystem has no Editor play-mode preview yet; it " +
                "simulates on the console. Build and run to see it.");
        }
    }
}
