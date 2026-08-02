using System;
using UnityEngine.Internal;

namespace UnityEngine
{
    // Unity's AsyncOperation, backed by the native SceneLoader (M10 task 5).
    //
    // Coroutine use is the point: `yield return SceneManager.LoadSceneAsync(x)`
    // suspends until the load finishes, exactly as it does in the Editor. The
    // pump in UnityEngine.Internal.Runtime knows to wait on this type, and it
    // advances the load once per frame so the operation makes progress even
    // while the game keeps rendering a loading screen.
    //
    // Absent from Unity's version on purpose: `priority` (there is one load
    // in flight at a time, so there is nothing to order).
    public class AsyncOperation : YieldInstruction
    {
        private Action<AsyncOperation> m_Completed;
        private bool m_CompletedRaised;

        internal AsyncOperation()
        {
        }

        public virtual bool isDone => false;
        public virtual float progress => 0f;

        // Setting this false parks a finished read at progress 0.9 without
        // swapping the world; setting it true again lets the next frame
        // finish the job.
        public virtual bool allowSceneActivation
        {
            get => true;
            set { }
        }

        // Unity raises this once, on the frame the operation completes. A
        // handler added after completion runs immediately, which is also what
        // Unity does.
        public event Action<AsyncOperation> completed
        {
            add
            {
                if (m_CompletedRaised)
                    value(this);
                else
                    m_Completed += value;
            }
            remove { m_Completed -= value; }
        }

        internal void RaiseCompleted()
        {
            if (m_CompletedRaised)
                return;
            m_CompletedRaised = true;
            Action<AsyncOperation> handler = m_Completed;
            m_Completed = null;
            if (handler == null)
                return;
            try
            {
                handler(this);
            }
            catch (Exception e)
            {
                Debug.LogError("AsyncOperation.completed handler threw: " + e);
            }
        }
    }

    // The scene-load flavour. Everything it reports comes from the native
    // loader, so there is no managed copy of the state to drift out of sync.
    internal sealed class SceneLoadOperation : AsyncOperation
    {
        // Mirrors ps2ur::scene::LoadState.
        internal const int StateIdle = 0;
        internal const int StateReading = 1;
        internal const int StateParsing = 2;
        internal const int StateReady = 3;
        internal const int StateFailed = 4;

        private bool m_AllowActivation = true;

        // A load that never started (bad path, no scene buffer bound) is
        // reported as done-and-failed rather than hanging the coroutine that
        // is waiting on it forever.
        internal bool StartFailed;

        public override bool isDone
        {
            get
            {
                if (StartFailed)
                    return true;
                int state = Native.ps2ur_scene_load_state();
                return state == StateReady || state == StateFailed;
            }
        }

        public override float progress =>
            StartFailed ? 1f : Native.ps2ur_scene_load_progress();

        public override bool allowSceneActivation
        {
            get => m_AllowActivation;
            set
            {
                if (m_AllowActivation == value)
                    return;
                m_AllowActivation = value;
                Native.ps2ur_scene_load_set_allow_activation(value ? 1 : 0);
            }
        }
    }
}
