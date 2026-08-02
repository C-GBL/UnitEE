using UnityEngine.Internal;

namespace UnityEngine
{
    // Unity's Rigidbody over the native body table (plan section 7.1, M11).
    //
    // SUBSET DISCIPLINE (ADR-007). Absent, each because the solver behind
    // this genuinely does not have it -- and a property that read back what
    // you set while changing nothing would be worse than a compile error:
    //   - constraints beyond freezeRotation (no constraint solver)
    //   - interpolation and collisionDetectionMode (the step is fixed at the
    //     frame rate, so there is nothing to interpolate between)
    //   - inertiaTensor, centerOfMass (rotation is not solved from contacts)
    //   - AddForce modes other than Force, AddExplosionForce, AddTorque
    //   - Sleep()/WakeUp() as explicit calls; sleeping is automatic and
    //     setting a velocity or adding a force wakes the body.
    public sealed class Rigidbody : Component
    {
        private int m_Body = -1;
        private float m_Mass = 1f;
        private float m_LinearDamping;
        private float m_AngularDamping = 0.05f;
        private bool m_UseGravity = true;
        private bool m_IsKinematic;
        private bool m_FreezeRotation;

        // Created against the collider already on this entity.
        //
        // A collider index of -1 means this entity has none. In Unity such a
        // body still falls; here it CANNOT, and saying so is the whole point
        // of this branch. The solver addresses positions by collider index
        // (phys::step takes a per-collider transform array), so a body with
        // no collider integrates a velocity into a slot that does not exist
        // and its transform never moves. A silent no-op is the worst
        // possible outcome, so it is a console error naming the fix.
        internal void Bind(int colliderIndex)
        {
            if (colliderIndex < 0)
            {
                Debug.LogError(
                    "Rigidbody on '" + (gameObject != null ? gameObject.name : "?") +
                    "' has no Collider, so it cannot simulate: this solver moves " +
                    "bodies through their collider. Add a BoxCollider, " +
                    "SphereCollider or CapsuleCollider. Deviation 23 in " +
                    "docs/supported-api.md.");
                return;
            }
            m_Body = Native.ps2ur_phys_add_body(colliderIndex, m_Mass,
                                                m_UseGravity ? 1 : 0,
                                                m_IsKinematic ? 1 : 0);
            if (m_Body < 0)
            {
                Debug.LogError("Rigidbody: the native body table is full");
                return;
            }
            // add_body does not carry these, so they are pushed after: a body
            // that took the scene's mass but the runtime's default damping
            // would drift from the Editor in a way nobody would think to
            // look for.
            Native.ps2ur_phys_body_set_damping(m_Body, m_LinearDamping,
                                               m_AngularDamping);
            PushFlags();
        }

        private void PushFlags()
        {
            if (m_Body >= 0)
                Native.ps2ur_phys_body_set_flags(m_Body, m_UseGravity ? 1 : 0,
                                                 m_IsKinematic ? 1 : 0,
                                                 m_FreezeRotation ? 1 : 0);
        }

        internal int BodyIndex => m_Body;

        public float mass
        {
            get => m_Mass;
            set => m_Mass = value > 0f ? value : 0.0001f;
        }

        public float linearDamping
        {
            get => m_LinearDamping;
            set
            {
                m_LinearDamping = value > 0f ? value : 0f;
                if (m_Body >= 0)
                    Native.ps2ur_phys_body_set_damping(m_Body, m_LinearDamping,
                                                       m_AngularDamping);
            }
        }

        public float angularDamping
        {
            get => m_AngularDamping;
            set
            {
                m_AngularDamping = value > 0f ? value : 0f;
                if (m_Body >= 0)
                    Native.ps2ur_phys_body_set_damping(m_Body, m_LinearDamping,
                                                       m_AngularDamping);
            }
        }

        // Unity 6 renamed drag/angularDrag to linearDamping/angularDamping and
        // kept the old names as obsolete aliases. Both exist here for the same
        // reason they exist there: existing scripts still say 'drag'.
        [System.Obsolete("Use linearDamping instead. (UnityUpgradable) -> linearDamping")]
        public float drag
        {
            get => linearDamping;
            set => linearDamping = value;
        }

        [System.Obsolete("Use angularDamping instead. (UnityUpgradable) -> angularDamping")]
        public float angularDrag
        {
            get => angularDamping;
            set => angularDamping = value;
        }

        public bool useGravity
        {
            get => m_UseGravity;
            set { m_UseGravity = value; PushFlags(); }
        }

        public bool isKinematic
        {
            get => m_IsKinematic;
            set { m_IsKinematic = value; PushFlags(); }
        }

        // The one constraint this solver has. Unity's RigidbodyConstraints
        // (position freezing, per-axis rotation freezing) needs a constraint
        // solver, which ADR-009 deliberately does not build.
        public bool freezeRotation
        {
            get => m_FreezeRotation;
            set { m_FreezeRotation = value; PushFlags(); }
        }

        public Vector3 velocity
        {
            get
            {
                if (m_Body < 0)
                    return Vector3.zero;
                Vector3 v = default;
                Native.ps2ur_phys_body_get_velocity(m_Body, ref v);
                return v;
            }
            set
            {
                if (m_Body >= 0)
                    Native.ps2ur_phys_body_set_velocity(m_Body, value.x, value.y,
                                                        value.z);
            }
        }

        // Force mode only, and the accumulator is cleared every step -- so
        // this belongs in FixedUpdate, exactly as it does in Unity.
        public void AddForce(Vector3 force)
        {
            if (m_Body >= 0)
                Native.ps2ur_phys_body_add_force(m_Body, force.x, force.y,
                                                 force.z);
        }

        public void MovePosition(Vector3 position)
        {
            // Kinematic movement is a transform write, not a solver input.
            if (transform != null)
                transform.position = position;
        }
    }

    // Unity's CharacterController over the native swept capsule.
    //
    // Absent: detectCollisions, enableOverlapRecovery, minMoveDistance and
    // SimpleMove (which needs gravity integration this controller leaves to
    // the caller, deliberately -- see the Move docs below).
    public sealed class CharacterController : Component
    {
        private int m_Index = -1;
        private float m_Radius = 0.5f;
        private float m_Height = 2f;
        private float m_SlopeLimit = 45f;
        private float m_StepOffset = 0.3f;

        internal void Bind(int entityHandle)
        {
            m_Index = Native.ps2ur_phys_add_character(entityHandle, m_Radius,
                                                      m_Height, m_SlopeLimit,
                                                      m_StepOffset);
            if (m_Index < 0)
                Debug.LogError("CharacterController: the native table is full");
        }

        public float radius { get => m_Radius; set => m_Radius = value; }
        public float height { get => m_Height; set => m_Height = value; }
        public float slopeLimit { get => m_SlopeLimit; set => m_SlopeLimit = value; }
        public float stepOffset { get => m_StepOffset; set => m_StepOffset = value; }

        public bool isGrounded =>
            m_Index >= 0 && Native.ps2ur_phys_character_grounded(m_Index) != 0;

        // Moves by 'motion' in WORLD units, sliding along whatever it hits.
        // Gravity is the caller's job: Unity's Move does not apply it either,
        // and a controller that secretly did would fight every game that
        // applies its own.
        public CollisionFlags Move(Vector3 motion)
        {
            if (m_Index < 0 || gameObject == null)
                return CollisionFlags.None;
            return (CollisionFlags)Native.ps2ur_phys_move_character(
                m_Index, gameObject.Handle, motion.x, motion.y, motion.z);
        }
    }

    // Unity's CollisionFlags, with its values.
    [System.Flags]
    public enum CollisionFlags
    {
        None = 0,
        Sides = 1,
        Above = 2,
        Below = 4,
        CollidedSides = 1,
        CollidedAbove = 2,
        CollidedBelow = 4,
    }
}
