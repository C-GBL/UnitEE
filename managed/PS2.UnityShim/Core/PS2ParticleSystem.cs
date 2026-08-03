using UnityEngine;
using UnityEngine.Internal;

namespace Ps2.Runtime
{
    // The console side of PS2ParticleSystem (M12.5 task 4, ADR-011).
    //
    // The full class name matches the Editor authoring component in the
    // com.example.ps2 package's Runtime assembly, so a user script that
    // compiles against one compiles against the other -- same rule as the
    // rest of the shim, one namespace over.
    //
    // Deliberately NOT Unity's ParticleSystem: the emitter is authored data
    // (rate, burst, shape, ramps) baked at export, and this class only
    // drives it. There are no modules, no curves, no runtime emitter
    // mutation -- the validator names this class when it meets Unity's own
    // ParticleSystem, and ADR-011 records why.
    public sealed class PS2ParticleSystem : Behaviour
    {
        private int Handle => gameObject != null ? gameObject.Handle : 0;

        // Starts emission and fires the authored burst.
        public void Play() => Native.ps2ur_particles_play(Handle);

        // Emission ceases; live particles play out (Unity's Stop).
        public void Stop() => Native.ps2ur_particles_stop(Handle);

        // Spawns immediately, up to the pool's remaining capacity.
        public void Emit(int count) => Native.ps2ur_particles_emit(Handle, count);

        public bool isPlaying => Native.ps2ur_particles_is_playing(Handle) != 0;

        public int particleCount => Native.ps2ur_particles_count(Handle);
    }
}
