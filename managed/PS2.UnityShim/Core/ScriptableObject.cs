using System;

namespace UnityEngine
{
    // Assets-only per plan section 7.1: instances are either deserialized from
    // the p2b container (TODO(spec missing: section 10)) or created in code.
    public class ScriptableObject : Object
    {
        public static T CreateInstance<T>() where T : ScriptableObject =>
            Activator.CreateInstance<T>();

        public static ScriptableObject CreateInstance(Type type) =>
            (ScriptableObject)Activator.CreateInstance(type);
    }
}
