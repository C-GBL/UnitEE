using System.Collections.Generic;

namespace UnityEngine
{
    // TODO(native-backing): plain managed fields for now, chosen deliberately
    // so the Editor play-mode stand-in can reuse this implementation as-is.
    // On target these become views over the native scene data owned by ps2ur
    // (plan section 6); the switch happens behind these same properties.
    // TODO(spec missing: section 12): the managed shim spec defines the exact
    // native-view mechanism.
    public class Transform : Component
    {
        private Vector3 m_LocalPosition = Vector3.zero;
        private Quaternion m_LocalRotation = Quaternion.identity;
        private Vector3 m_LocalScale = Vector3.one;
        private Transform m_Parent;
        private readonly List<Transform> m_Children = new List<Transform>();

        // Created only by GameObject; Unity forbids AddComponent<Transform>.
        internal Transform() { }

        public Vector3 localPosition
        {
            get => m_LocalPosition;
            set => m_LocalPosition = value;
        }

        public Quaternion localRotation
        {
            get => m_LocalRotation;
            set => m_LocalRotation = value;
        }

        public Vector3 localScale
        {
            get => m_LocalScale;
            set => m_LocalScale = value;
        }

        public Vector3 position
        {
            get => m_Parent == null ? m_LocalPosition : m_Parent.TransformPoint(m_LocalPosition);
            set => m_LocalPosition = m_Parent == null ? value : m_Parent.InverseTransformPoint(value);
        }

        public Quaternion rotation
        {
            get => m_Parent == null ? m_LocalRotation : m_Parent.rotation * m_LocalRotation;
            set => m_LocalRotation = m_Parent == null ? value : Quaternion.Inverse(m_Parent.rotation) * value;
        }

        // NOTE: componentwise product up the chain; like Unity's lossyScale it
        // is not meaningful under rotated non-uniform scale in a hierarchy.
        public Vector3 lossyScale =>
            m_Parent == null ? m_LocalScale : Vector3.Scale(m_Parent.lossyScale, m_LocalScale);

        public Vector3 forward => rotation * Vector3.forward;
        public Vector3 right => rotation * Vector3.right;
        public Vector3 up => rotation * Vector3.up;

        public Transform parent
        {
            get => m_Parent;
            set => SetParent(value, true);
        }

        public void SetParent(Transform newParent) => SetParent(newParent, true);

        public void SetParent(Transform newParent, bool worldPositionStays)
        {
            if (ReferenceEquals(newParent, m_Parent)) return;
            // TODO(native-backing): cycle detection deferred to the native
            // scene graph, which owns hierarchy validity on target.
            Vector3 keepPos = default;
            Quaternion keepRot = default;
            if (worldPositionStays)
            {
                keepPos = position;
                keepRot = rotation;
            }
            if (m_Parent != null) m_Parent.m_Children.Remove(this);
            m_Parent = newParent;
            if (m_Parent != null) m_Parent.m_Children.Add(this);
            if (worldPositionStays)
            {
                position = keepPos;
                rotation = keepRot;
            }
        }

        public int childCount => m_Children.Count;

        public Transform GetChild(int index) => m_Children[index];

        public Transform root => m_Parent == null ? this : m_Parent.root;

        // Local space of this transform -> world space.
        public Vector3 TransformPoint(Vector3 point)
        {
            Vector3 p = new Vector3(point.x * m_LocalScale.x, point.y * m_LocalScale.y, point.z * m_LocalScale.z);
            p = m_LocalRotation * p + m_LocalPosition;
            return m_Parent == null ? p : m_Parent.TransformPoint(p);
        }

        // World space -> local space of this transform.
        public Vector3 InverseTransformPoint(Vector3 point)
        {
            Vector3 p = m_Parent == null ? point : m_Parent.InverseTransformPoint(point);
            p = Quaternion.Inverse(m_LocalRotation) * (p - m_LocalPosition);
            return new Vector3(SafeDiv(p.x, m_LocalScale.x), SafeDiv(p.y, m_LocalScale.y), SafeDiv(p.z, m_LocalScale.z));
        }

        // Direction transforms are rotation-only (Unity semantics).
        public Vector3 TransformDirection(Vector3 direction) => rotation * direction;
        public Vector3 InverseTransformDirection(Vector3 direction) => Quaternion.Inverse(rotation) * direction;

        public Matrix4x4 localToWorldMatrix
        {
            get
            {
                Matrix4x4 local = Matrix4x4.TRS(m_LocalPosition, m_LocalRotation, m_LocalScale);
                return m_Parent == null ? local : m_Parent.localToWorldMatrix * local;
            }
        }

        private static float SafeDiv(float v, float s) => s != 0f ? v / s : 0f;
    }
}
