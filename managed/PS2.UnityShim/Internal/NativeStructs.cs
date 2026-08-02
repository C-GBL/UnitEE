using System.Runtime.InteropServices;

namespace UnityEngine.Internal
{
    // Transport structs for the physics boundary (M11 task 3).
    //
    // These mirror P2RaycastHit and P2Contact in
    // runtime/src/bridge/generated_bridge.h EXACTLY -- field order, field
    // types, no padding. The C++ side carries generated static_asserts on
    // every offset, so a change to bridge-api.json breaks the native build
    // loudly; this file is the managed half of that contract and must be
    // edited alongside it.
    //
    // Flat floats rather than Vector3 fields on purpose: the boundary
    // forbids nested structs (ADR-002 -- a nested layout needs a padding
    // story neither side has agreed). The Unity-shaped RaycastHit that games
    // actually use is built from this one in Physics.cs.
    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeRaycastHit
    {
        public float pointX;
        public float pointY;
        public float pointZ;
        public float normalX;
        public float normalY;
        public float normalZ;
        public float distance;
        public int collider;
        public int triangle;
        public int layer;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeContact
    {
        public float pointX;
        public float pointY;
        public float pointZ;
        public float normalX;
        public float normalY;
        public float normalZ;
        public int colliderA;
        public int colliderB;
        // ps2ur::phys::ContactPhase: 0 Enter, 1 Stay, 2 Exit.
        public int phase;
        public int isTrigger;
        public float separation;
    }
}
