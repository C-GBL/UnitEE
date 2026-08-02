using System;
using System.Collections;
using UnityEngine.Internal;

namespace UnityEngine
{
    // Coroutine surface (plan section 7.1 "Lifecycle"). The scheduler that
    // interprets these is UnityEngine.Internal.Runtime's IEnumerator pump
    // (plan M7 task 5); supported yields are WaitForSeconds,
    // WaitForFixedUpdate, WaitUntil (CustomYieldInstruction), and null.

    public class YieldInstruction
    {
    }

    public sealed class Coroutine : YieldInstruction
    {
        internal readonly Runtime.CoroutineState m_State;

        internal Coroutine(Runtime.CoroutineState state)
        {
            m_State = state;
        }
    }

    public sealed class WaitForSeconds : YieldInstruction
    {
        internal readonly float m_Seconds;

        public WaitForSeconds(float seconds)
        {
            m_Seconds = seconds;
        }
    }

    public sealed class WaitForFixedUpdate : YieldInstruction
    {
    }

    public abstract class CustomYieldInstruction : IEnumerator
    {
        public abstract bool keepWaiting { get; }

        public object Current => null;
        public bool MoveNext() => keepWaiting;
        public void Reset() { }
    }

    public sealed class WaitUntil : CustomYieldInstruction
    {
        private readonly Func<bool> m_Predicate;

        public WaitUntil(Func<bool> predicate)
        {
            m_Predicate = predicate;
        }

        public override bool keepWaiting => !m_Predicate();
    }
}
