using System;

namespace UnityEngine
{
    // Base of the Unity object model (plan section 7.1 "Object model").
    // Compiling skeleton: identity and the famous null/destroyed equality
    // semantics are real; lifetime operations require the native runtime and
    // throw for now.
    public class Object
    {
        private static int s_NextInstanceId = 1;

        private readonly int m_InstanceId;
        private string m_Name = "";

        // TODO(native-backing): on target the source of truth for "destroyed"
        // is the ps2ur entity table; this managed flag is a placeholder that
        // the Editor play-mode stand-in and the runtime bridge will both set.
        internal bool m_Destroyed;

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

        public static void Destroy(Object obj) => Destroy(obj, 0f);

        public static void Destroy(Object obj, float t)
        {
            throw new NotImplementedException("TODO(native-backing): Object.Destroy requires the ps2ur runtime (or the Editor stand-in scheduler).");
        }

        public static Object Instantiate(Object original)
        {
            throw new NotImplementedException("TODO(native-backing): Object.Instantiate requires the ps2ur runtime.");
        }

        public static T Instantiate<T>(T original) where T : Object
        {
            throw new NotImplementedException("TODO(native-backing): Object.Instantiate requires the ps2ur runtime.");
        }

        public static T Instantiate<T>(T original, Vector3 position, Quaternion rotation) where T : Object
        {
            throw new NotImplementedException("TODO(native-backing): Object.Instantiate requires the ps2ur runtime.");
        }

        // Unity's destroyed-object semantics: a destroyed (or genuinely null)
        // reference compares equal to null and converts to false.
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

        private static bool IsNullOrDestroyed(Object o) => ReferenceEquals(o, null) || o.m_Destroyed;
    }
}
