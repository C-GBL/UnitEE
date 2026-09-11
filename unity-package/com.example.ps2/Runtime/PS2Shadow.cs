using UnityEngine;

namespace Ps2.Runtime
{
    /// <summary>
    /// A shadow the PS2 build target actually draws (M14). Two techniques,
    /// both the era's:
    ///
    ///   Blob      -- a soft dark disc on the ground under this object,
    ///                found by a ray straight down, faded by height. Sixteen
    ///                triangles; put one under anything that moves.
    ///   Projected -- the blob, plus this object's lit meshes and skinned
    ///                renderers drawn again flattened onto that ground along
    ///                the scene's strongest directional light. A real
    ///                silhouette, at the cost of drawing the object twice:
    ///                use it for the player and a few key things, not a
    ///                crowd. Unlit and textured-unlit meshes cast a blob
    ///                only (their colour cannot be turned off by constants).
    ///
    /// The console-side class (in PS2.UnityShim) has the same full name so
    /// scripts compile in both worlds. Fields are baked at export.
    /// </summary>
    [AddComponentMenu("PS2/PS2 Shadow")]
    [DisallowMultipleComponent]
    public sealed class PS2Shadow : MonoBehaviour
    {
        public enum Mode
        {
            Blob = 0,
            Projected = 1,
        }

        [Tooltip("Blob only, or blob plus the object's meshes projected onto the ground.")]
        public Mode mode = Mode.Blob;

        [Tooltip("Radius of the blob on the ground, in world units.")]
        public float radius = 0.5f;

        [Tooltip("How dark the shadow is: 0 none, 1 black.")]
        [Range(0f, 1f)] public float strength = 0.6f;

        [Tooltip("Height above the ground at which the blob has faded to nothing.")]
        public float maxHeight = 3f;

        private void OnDrawGizmosSelected()
        {
            Gizmos.color = new Color(0f, 0f, 0f, 0.5f);
            Vector3 origin = transform.position + Vector3.up * 0.1f;
            if (Physics.Raycast(origin, Vector3.down, out RaycastHit hit, maxHeight + 1f))
                Gizmos.DrawWireSphere(hit.point, radius);
        }
    }
}
