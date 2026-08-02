using UnityEngine.Internal;

namespace UnityEngine
{
    // Native-backed transform (plan section 12.2): local TRS and the
    // hierarchy live in ps2ur's entity table; every property here is a view
    // through the bridge. World-space math is composed on the MANAGED side
    // from the local values, exactly the algorithms Unity uses -- that is
    // what the M7 golden-trace parity test measures. The renderer consumes
    // the native world-matrix pass computed from the same local values.
    public class Transform : Component
    {
        internal Transform(GameObject owner)
        {
            Attach(owner);
        }

        internal int Handle => gameObjectInternal.Handle;

        // gameObject is null-semantics-filtered; the transform itself needs
        // the raw owner for handle access even mid-destruction.
        private GameObject gameObjectInternal => base.RawGameObject;

        public Vector3 localPosition
        {
            get { Native.ps2ur_tf_get_local_position(Handle, out Vector3 v); return v; }
            set => Native.ps2ur_tf_set_local_position(Handle, value.x, value.y, value.z);
        }

        public Quaternion localRotation
        {
            get { Native.ps2ur_tf_get_local_rotation(Handle, out Quaternion q); return q; }
            set => Native.ps2ur_tf_set_local_rotation(Handle, value.x, value.y, value.z, value.w);
        }

        public Vector3 localScale
        {
            get { Native.ps2ur_tf_get_local_scale(Handle, out Vector3 v); return v; }
            set => Native.ps2ur_tf_set_local_scale(Handle, value.x, value.y, value.z);
        }

        public Vector3 position
        {
            get
            {
                Transform p = parent;
                return p == null ? localPosition : p.TransformPoint(localPosition);
            }
            set
            {
                Transform p = parent;
                localPosition = p == null ? value : p.InverseTransformPoint(value);
            }
        }

        public Quaternion rotation
        {
            get
            {
                Transform p = parent;
                return p == null ? localRotation : p.rotation * localRotation;
            }
            set
            {
                Transform p = parent;
                localRotation = p == null ? value : Quaternion.Inverse(p.rotation) * value;
            }
        }

        // NOTE: componentwise product up the chain; like Unity's lossyScale it
        // is not meaningful under rotated non-uniform scale in a hierarchy.
        public Vector3 lossyScale
        {
            get
            {
                Transform p = parent;
                return p == null ? localScale : Vector3.Scale(p.lossyScale, localScale);
            }
        }

        public Vector3 forward => rotation * Vector3.forward;
        public Vector3 right => rotation * Vector3.right;
        public Vector3 up => rotation * Vector3.up;

        public Transform parent
        {
            get
            {
                int parentHandle = Native.ps2ur_tf_get_parent(Handle);
                if (parentHandle == 0)
                    return null;
                GameObject go = Runtime.GetOrCreateWrapper(parentHandle);
                return go != null ? go.transform : null;
            }
            set => SetParent(value, true);
        }

        public void SetParent(Transform newParent) => SetParent(newParent, true);

        public void SetParent(Transform newParent, bool worldPositionStays)
        {
            Transform current = parent;
            if (ReferenceEquals(newParent, current)) return;
            if (newParent != null && ReferenceEquals(newParent, this)) return;

            Vector3 keepPos = default;
            Quaternion keepRot = default;
            if (worldPositionStays)
            {
                keepPos = position;
                keepRot = rotation;
            }
            Native.ps2ur_tf_set_parent(Handle, newParent != null ? newParent.Handle : 0);
            if (worldPositionStays)
            {
                position = keepPos;
                rotation = keepRot;
            }
        }

        public int childCount => Native.ps2ur_tf_child_count(Handle);

        public Transform GetChild(int index)
        {
            int childHandle = Native.ps2ur_tf_get_child(Handle, index);
            if (childHandle == 0)
                return null;
            GameObject go = Runtime.GetOrCreateWrapper(childHandle);
            return go != null ? go.transform : null;
        }

        public Transform root
        {
            get
            {
                Transform t = this;
                for (Transform p = t.parent; p != null; p = t.parent)
                    t = p;
                return t;
            }
        }

        // Rotates by Euler angles. Space.Self (the default in Unity) composes
        // on the right of the LOCAL rotation.
        public void Rotate(float xAngle, float yAngle, float zAngle)
        {
            localRotation = localRotation * Quaternion.Euler(xAngle, yAngle, zAngle);
        }

        public void Rotate(Vector3 eulers) => Rotate(eulers.x, eulers.y, eulers.z);

        public void Translate(Vector3 translation)
        {
            // Space.Self: the translation is in local orientation.
            position += rotation * translation;
        }

        public void Translate(float x, float y, float z) => Translate(new Vector3(x, y, z));

        // Local space of this transform -> world space.
        public Vector3 TransformPoint(Vector3 point)
        {
            Vector3 s = localScale;
            Vector3 p = new Vector3(point.x * s.x, point.y * s.y, point.z * s.z);
            p = localRotation * p + localPosition;
            Transform par = parent;
            return par == null ? p : par.TransformPoint(p);
        }

        // World space -> local space of this transform.
        public Vector3 InverseTransformPoint(Vector3 point)
        {
            Transform par = parent;
            Vector3 p = par == null ? point : par.InverseTransformPoint(point);
            p = Quaternion.Inverse(localRotation) * (p - localPosition);
            Vector3 s = localScale;
            return new Vector3(SafeDiv(p.x, s.x), SafeDiv(p.y, s.y), SafeDiv(p.z, s.z));
        }

        // Direction transforms are rotation-only (Unity semantics).
        public Vector3 TransformDirection(Vector3 direction) => rotation * direction;
        public Vector3 InverseTransformDirection(Vector3 direction) => Quaternion.Inverse(rotation) * direction;

        public Matrix4x4 localToWorldMatrix
        {
            get
            {
                Matrix4x4 local = Matrix4x4.TRS(localPosition, localRotation, localScale);
                Transform par = parent;
                return par == null ? local : par.localToWorldMatrix * local;
            }
        }

        private static float SafeDiv(float v, float s) => s != 0f ? v / s : 0f;
    }
}
