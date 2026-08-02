using System;
using UnityEngine.Internal;

namespace UnityEngine
{
    // Base of the Unity object model (plan section 7.1 "Object model").
    // Identity plus the famous null/destroyed equality semantics: a destroyed
    // object compares equal to null and converts to false, because scripts
    // depend on it (plan section 9 M7 task 3). For native-backed objects the
    // truth is the entity table's generation check, surfaced through
    // IsDestroyedInternal overrides.
    public class Object
    {
        private static int s_NextInstanceId = 1;

        private readonly int m_InstanceId;
        private string m_Name = "";
        private bool m_Destroyed;

        public Object()
        {
            m_InstanceId = s_NextInstanceId++;
        }

        public string name
        {
            get => m_Name;
            set => m_Name = value ?? "";
        }

        public int GetInstanceID() => m_InstanceId;

        internal virtual bool IsDestroyedInternal => m_Destroyed;

        internal void MarkDestroyed() => m_Destroyed = true;

        // Destruction is deferred to the end of the current frame, exactly
        // like Unity: scripts running later this frame still see the object.
        public static void Destroy(Object obj) => Destroy(obj, 0f);

        public static void Destroy(Object obj, float t)
        {
            if (t != 0f)
                throw new NotSupportedException(
                    "Object.Destroy with a delay is not supported on PS2 yet; " +
                    "schedule it from a coroutine instead.");
            if (obj != null)
                Runtime.DeferDestroy(obj);
        }

        public static Object Instantiate(Object original)
        {
            throw new NotSupportedException(
                "Object.Instantiate is not supported on PS2 yet (no prefab " +
                "instantiation until the asset pipeline carries prefabs).");
        }

        public static T Instantiate<T>(T original) where T : Object
        {
            throw new NotSupportedException(
                "Object.Instantiate is not supported on PS2 yet (no prefab " +
                "instantiation until the asset pipeline carries prefabs).");
        }

        public static T Instantiate<T>(T original, Vector3 position, Quaternion rotation) where T : Object
        {
            throw new NotSupportedException(
                "Object.Instantiate is not supported on PS2 yet (no prefab " +
                "instantiation until the asset pipeline carries prefabs).");
        }

        public static bool operator ==(Object x, Object y) => CompareBaseObjects(x, y);
        public static bool operator !=(Object x, Object y) => !CompareBaseObjects(x, y);

        public static implicit operator bool(Object exists) => !IsNullOrDestroyed(exists);

        public override bool Equals(object other) => CompareBaseObjects(this, other as Object);
        public override int GetHashCode() => m_InstanceId;
        public override string ToString() => m_Name.Length != 0 ? m_Name : GetType().Name;

        private static bool CompareBaseObjects(Object x, Object y)
        {
            bool xNull = IsNullOrDestroyed(x);
            bool yNull = IsNullOrDestroyed(y);
            if (xNull && yNull) return true;
            if (xNull || yNull) return false;
            return ReferenceEquals(x, y);
        }

        private static bool IsNullOrDestroyed(Object o) =>
            ReferenceEquals(o, null) || o.IsDestroyedInternal;
    }
}
