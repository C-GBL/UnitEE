using System;
using UnityEngine.Internal;

namespace UnityEngine
{
    // What a query hit. Unity's RaycastHit is a struct with lazily-computed
    // properties over an internal blob; this is the same public surface over
    // the flat native transport struct.
    //
    // Absent from Unity's version on purpose: barycentricCoordinate,
    // textureCoord (there are no UVs on a collision mesh -- it is baked
    // separately from the render mesh), rigidbody and transform as object
    // references (colliderIndex is the identity here), and lightmapCoord.
    public struct RaycastHit
    {
        public Vector3 point;
        public Vector3 normal;
        public float distance;
        // Index into the native collider table, or -1 for a hit on baked
        // static geometry. Static geometry has no Collider component to
        // return, which is a real difference from the Editor.
        public int colliderIndex;
        // Index of the static triangle hit, or -1 for a collider hit.
        public int triangleIndex;
        public int layer;

        public bool IsStaticGeometry => colliderIndex < 0;

        internal static RaycastHit From(NativeRaycastHit n)
        {
            RaycastHit hit;
            hit.point = new Vector3(n.pointX, n.pointY, n.pointZ);
            hit.normal = new Vector3(n.normalX, n.normalY, n.normalZ);
            hit.distance = n.distance;
            hit.colliderIndex = n.collider;
            hit.triangleIndex = n.triangle;
            hit.layer = n.layer;
            return hit;
        }
    }

    // A ray, as Unity spells it.
    public struct Ray
    {
        public Vector3 origin;
        public Vector3 direction;

        public Ray(Vector3 origin, Vector3 direction)
        {
            this.origin = origin;
            this.direction = direction.normalized;
        }

        public Vector3 GetPoint(float distance) => origin + direction * distance;
    }

    // Unity-shaped physics queries (plan section 7.1, M11).
    //
    // NOT PHYSX, AND NOT PRETENDING TO BE. The plan calls the whole module
    // "deliberately minimal and deliberately not PhysX-compatible"; contact
    // behaviour, resting jitter and stacking all differ from the Editor. The
    // full list is in docs/supported-api.md. What matches exactly is the API
    // shape, so gameplay code compiles and reads the same.
    //
    // Absent on purpose, each because it would need a solver this runtime
    // does not have: RaycastAll and OverlapBox/Capsule (allocating query
    // variants -- OverlapSphere into a caller-owned array is provided
    // instead, which is the NonAlloc form Unity added later and the only one
    // a console should use), CapsuleCast, ComputePenetration, joints, and
    // continuous collision detection settings.
    public static class Physics
    {
        public const int AllLayers = ~0;
        public const int DefaultRaycastLayers = ~0;

        public static Vector3 gravity
        {
            get => s_Gravity;
            set
            {
                s_Gravity = value;
                Native.ps2ur_phys_set_gravity(value.x, value.y, value.z);
            }
        }
        private static Vector3 s_Gravity = new Vector3(0f, -9.81f, 0f);

        public static bool Raycast(Vector3 origin, Vector3 direction,
                                   float maxDistance = float.PositiveInfinity,
                                   int layerMask = AllLayers)
        {
            return Raycast(origin, direction, out _, maxDistance, layerMask);
        }

        public static bool Raycast(Vector3 origin, Vector3 direction,
                                   out RaycastHit hitInfo,
                                   float maxDistance = float.PositiveInfinity,
                                   int layerMask = AllLayers)
        {
            NativeRaycastHit native = default;
            // Infinity is not a value the EE's floats handle predictably
            // (deviation 1), so an unbounded query becomes a very long one.
            float distance = float.IsInfinity(maxDistance) ? 1e6f : maxDistance;
            int hit = Native.ps2ur_phys_raycast(origin.x, origin.y, origin.z,
                                                direction.x, direction.y,
                                                direction.z, distance,
                                                layerMask, ref native);
            hitInfo = hit != 0 ? RaycastHit.From(native) : default;
            return hit != 0;
        }

        public static bool Raycast(Ray ray, out RaycastHit hitInfo,
                                   float maxDistance = float.PositiveInfinity,
                                   int layerMask = AllLayers)
        {
            return Raycast(ray.origin, ray.direction, out hitInfo, maxDistance,
                           layerMask);
        }

        public static bool SphereCast(Vector3 origin, float radius,
                                      Vector3 direction, out RaycastHit hitInfo,
                                      float maxDistance = float.PositiveInfinity,
                                      int layerMask = AllLayers)
        {
            NativeRaycastHit native = default;
            float distance = float.IsInfinity(maxDistance) ? 1e6f : maxDistance;
            int hit = Native.ps2ur_phys_spherecast(origin.x, origin.y, origin.z,
                                                   radius, direction.x,
                                                   direction.y, direction.z,
                                                   distance, layerMask,
                                                   ref native);
            hitInfo = hit != 0 ? RaycastHit.From(native) : default;
            return hit != 0;
        }

        // The NonAlloc form only. A console has no business allocating an
        // array per query, and Unity's own guidance is to use this one.
        // Returns how many were FOUND, which may exceed results.Length --
        // so a caller can tell it was truncated rather than silently
        // missing objects.
        public static int OverlapSphereNonAlloc(Vector3 position, float radius,
                                                int[] results,
                                                int layerMask = AllLayers)
        {
            int found = Native.ps2ur_phys_overlap_sphere(
                position.x, position.y, position.z, radius, layerMask);
            if (results != null)
            {
                int copy = found < results.Length ? found : results.Length;
                for (int i = 0; i < copy; i++)
                    results[i] = Native.ps2ur_phys_overlap_result(i);
            }
            return found;
        }

        // Unity's Physics.IgnoreLayerCollision, with its argument order.
        // Note the inversion: Unity says "ignore", the native layer says
        // "collide", and mixing those up is a classic source of a collision
        // matrix that does the opposite of what the inspector showed.
        public static void IgnoreLayerCollision(int layer1, int layer2,
                                                bool ignore = true)
        {
            Native.ps2ur_phys_set_layers_collide(layer1, layer2, ignore ? 0 : 1);
        }

        public static bool GetIgnoreLayerCollision(int layer1, int layer2) =>
            !s_LayerMatrix.Collides(layer1, layer2);

        // Mirrors what was set, so the getter does not need a native call.
        private static readonly LayerMatrix s_LayerMatrix = new LayerMatrix();

        private sealed class LayerMatrix
        {
            private readonly uint[] m_Rows = new uint[32];

            public LayerMatrix()
            {
                for (int i = 0; i < 32; i++)
                    m_Rows[i] = 0xFFFFFFFFu;
            }

            public bool Collides(int a, int b)
            {
                if ((uint)a >= 32u || (uint)b >= 32u)
                    return false;
                return (m_Rows[a] & (1u << b)) != 0u;
            }

            public void Set(int a, int b, bool collide)
            {
                if ((uint)a >= 32u || (uint)b >= 32u)
                    return;
                if (collide)
                {
                    m_Rows[a] |= 1u << b;
                    m_Rows[b] |= 1u << a;
                }
                else
                {
                    m_Rows[a] &= ~(1u << b);
                    m_Rows[b] &= ~(1u << a);
                }
            }
        }

        internal static void RecordLayerCollision(int a, int b, bool collide) =>
            s_LayerMatrix.Set(a, b, collide);
    }
}
