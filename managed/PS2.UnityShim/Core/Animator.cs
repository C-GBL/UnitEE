using UnityEngine.Internal;

namespace UnityEngine
{
    // The managed face of the native animator (plan section 7.1
    // "Animation", M9 task 4).
    //
    // SUBSET DISCIPLINE (docs/development.md, ADR-007): every member here behaves as
    // Unity's does. That is why CrossFade -- whose Unity signature takes a
    // NORMALIZED transition duration relative to the destination clip -- is
    // absent, while CrossFadeInFixedTime, which takes seconds, is present
    // and exact. A member that looks like Unity's but measures time
    // differently would be worse than no member at all.
    //
    // State and parameter names are resolved through the same FNV-1a hash
    // the exporter baked into the controller, so no string crosses the
    // boundary (ADR-002).
    public sealed class Animator : Behaviour
    {
        private int Handle => gameObject != null ? gameObject.Handle : 0;

        // Jumps to a state immediately.
        public void Play(string stateName)
        {
            Native.ps2ur_animator_play(Handle, NameHash(stateName));
        }

        // Blends to a state over a duration in seconds.
        public void CrossFadeInFixedTime(string stateName, float fixedTransitionDuration)
        {
            Native.ps2ur_animator_crossfade(Handle, NameHash(stateName),
                                            fixedTransitionDuration);
        }

        public void SetTrigger(string name)
        {
            Native.ps2ur_animator_set_trigger(Handle, NameHash(name));
        }

        public void SetFloat(string name, float value)
        {
            Native.ps2ur_animator_set_float(Handle, NameHash(name), value);
        }

        public void SetBool(string name, bool value)
        {
            Native.ps2ur_animator_set_float(Handle, NameHash(name), value ? 1f : 0f);
        }

        // Unity exposes this as IsInTransition(int layerIndex); layer 0 is
        // the state machine's own layer, which is the only one a baked
        // controller drives.
        public bool IsInTransition(int layerIndex)
        {
            return layerIndex == 0 &&
                   Native.ps2ur_animator_is_blending(Handle) != 0;
        }

        // FNV-1a-64 truncated to 32 bits -- the exporter's P2bWriter.Fnv1a64
        // cast to uint. Both sides must agree exactly or a lookup silently
        // misses, so this is deliberately spelled out rather than shared.
        internal static uint NameHash(string name)
        {
            ulong h = 14695981039346656037UL;
            for (int i = 0; i < name.Length; i++)
            {
                h ^= (byte)name[i];
                h *= 1099511628211UL;
            }
            return (uint)h;
        }
    }
}
