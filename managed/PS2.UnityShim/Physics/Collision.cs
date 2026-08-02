using UnityEngine.Internal;

namespace UnityEngine
{
    // What OnCollisionEnter/Stay/Exit receives.
    //
    // Unity's Collision carries a contacts[] array and object references;
    // this carries the ONE contact point the solver produced (there is no
    // manifold -- see docs/supported-api.md) and collider indices rather
    // than Collider components, because baked static geometry has no
    // component to point at.
    public sealed class Collision
    {
        public Vector3 point;
        public Vector3 normal;
        public float separation;
        // The other side. -1 means baked static geometry -- a wall or the
        // ground, which has no GameObject in this runtime.
        public int otherColliderIndex;
        public GameObject gameObject;

        public bool IsStaticGeometry => otherColliderIndex < 0;
    }

    // Where a collider's identity lives. A "Collider" here is an index into
    // the native table plus the entity it rides on; the shapes themselves
    // are authored in the Editor and baked, never constructed at runtime.
    //
    // This is a real divergence from Unity, where you can gameObject.
    // AddComponent<BoxCollider>() at runtime. Baked collision is world-space
    // and precomputed (that is what makes the broadphase free), so adding a
    // shape mid-frame is not something this design supports. Documented
    // rather than half-implemented.
    public sealed class Collider : Component
    {
        internal int Index = -1;

        public bool isTrigger { get; internal set; }

        // The Rigidbody driving this collider, if any.
        public Rigidbody attachedRigidbody { get; internal set; }

        internal static Collider ForEntity(GameObject go)
        {
            if (go == null)
                return null;
            int index = Native.ps2ur_phys_collider_for_entity(go.Handle);
            if (index < 0)
                return null;
            Collider c = new Collider { Index = index };
            c.Attach(go);
            return c;
        }
    }
}
