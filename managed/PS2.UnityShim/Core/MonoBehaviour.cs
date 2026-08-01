using System;
using System.Collections;

namespace UnityEngine
{
    // Base class for user scripts. Lifecycle methods (Awake, Start, Update,
    // FixedUpdate, LateUpdate, OnEnable, OnDisable, OnDestroy - plan 7.1) are
    // discovered and dispatched by the runtime scheduler, not declared here,
    // exactly like real Unity.
    public class MonoBehaviour : Behaviour
    {
        // TODO(native-backing): the coroutine scheduler lives in the ps2ur
        // frame loop (plan section 6) / the Editor stand-in. Surface only.
        public Coroutine StartCoroutine(IEnumerator routine)
        {
            throw new NotImplementedException("TODO(native-backing): coroutine scheduler not available yet.");
        }

        public void StopCoroutine(Coroutine routine)
        {
            throw new NotImplementedException("TODO(native-backing): coroutine scheduler not available yet.");
        }

        public void StopCoroutine(IEnumerator routine)
        {
            throw new NotImplementedException("TODO(native-backing): coroutine scheduler not available yet.");
        }

        public void StopAllCoroutines()
        {
            throw new NotImplementedException("TODO(native-backing): coroutine scheduler not available yet.");
        }

        public static void print(object message) => Debug.Log(message);
    }
}
