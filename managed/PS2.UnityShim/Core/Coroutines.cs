using System;
using System.Collections;

namespace UnityEngine
{
    // Coroutine surface (plan section 7.1 "Lifecycle"). These are inert data
    // shells; the scheduler that interprets them lives in the ps2ur frame
    // loop / the Editor play-mode stand-in. TODO(native-backing).

    public class YieldInstruction
    {
    }

    public sealed class Coroutine : YieldInstruction
    {
        internal Coroutine() { }
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
