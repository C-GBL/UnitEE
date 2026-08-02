using System.Collections;
using UnityEngine.Internal;

namespace UnityEngine
{
    // Base class for user scripts. Lifecycle methods (Awake, Start, Update,
    // FixedUpdate, LateUpdate, OnEnable, OnDisable, OnDestroy - plan 7.1) are
    // discovered and dispatched by UnityEngine.Internal.Runtime, not declared
    // here, exactly like real Unity. Coroutines run on the managed scheduler
    // (plan M7 task 5), never on native threads.
    public class MonoBehaviour : Behaviour
    {
        public Coroutine StartCoroutine(IEnumerator routine)
        {
            return Runtime.StartCoroutine(this, routine);
        }

        public void StopCoroutine(Coroutine routine)
        {
            Runtime.StopCoroutine(routine);
        }

        public void StopAllCoroutines()
        {
            Runtime.StopAllCoroutines(this);
        }

        public static void print(object message) => Debug.Log(message);
    }
}
