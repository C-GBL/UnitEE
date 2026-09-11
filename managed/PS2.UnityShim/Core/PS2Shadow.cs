using UnityEngine;

namespace Ps2.Runtime
{
    /// <summary>
    /// The console side of PS2Shadow (M14): the same full name as the
    /// authoring component in the package, so GetComponent&lt;PS2Shadow&gt;()
    /// compiles in both worlds. The fields mirror what the scene exported;
    /// changing them at runtime does not reach the renderer (deviation 40).
    /// </summary>
    public sealed class PS2Shadow : Behaviour
    {
        public enum Mode
        {
            Blob = 0,
            Projected = 1,
        }

        public Mode mode = Mode.Blob;
        public float radius = 0.5f;
        public float strength = 0.6f;
        public float maxHeight = 3f;
    }
}
