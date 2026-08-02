using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;

namespace UnityEngine.Internal
{
    // The managed side of the M7 lifecycle driver (plan section 9 M7 task 4).
    //
    // The native frame loop makes exactly TWO kinds of calls into managed
    // code, both through il2cpp's runtime-invoke on this class:
    //   CreateScript(typeName, entityHandle)  - once per script component at
    //                                           scene load
    //   Tick(dt)                              - once per frame
    // Everything else (per-behaviour Update, coroutines, deferred Destroy)
    // fans out HERE, in managed code, so the boundary cost per frame is one
    // call, not one per object.
    //
    // Lifecycle methods are Unity-style magic methods: discovered per TYPE by
    // reflection once (walking the declared hierarchy like Unity does), then
    // bound per INSTANCE as delegates so steady-state dispatch is a delegate
    // call, not MethodInfo.Invoke. The game assembly ships with a link.xml
    // that preserves it wholesale, which is what makes both Type.GetType and
    // this reflection safe under aggressive stripping.
    internal static class Runtime
    {
        private sealed class TypeMethods
        {
            public MethodInfo Awake, OnEnable, Start, Update, FixedUpdate;
            public MethodInfo LateUpdate, OnDisable, OnDestroy;
        }

        internal sealed class BehaviourState
        {
            public MonoBehaviour Behaviour;
            public Action Awake, OnEnable, Start, Update, FixedUpdate;
            public Action LateUpdate, OnDisable, OnDestroy;
            public bool AwakeRan, StartRan, EnabledRan;
        }

        internal sealed class CoroutineState
        {
            public IEnumerator Routine;
            public MonoBehaviour Owner;
            public float SleepSeconds;
            public bool WaitingForFixedUpdate;
            public bool Done;
        }

        private static readonly Dictionary<Type, TypeMethods> s_TypeCache =
            new Dictionary<Type, TypeMethods>();
        private static readonly Dictionary<int, GameObject> s_WrapperByHandle =
            new Dictionary<int, GameObject>();
        private static readonly List<BehaviourState> s_Behaviours =
            new List<BehaviourState>();
        private static readonly List<BehaviourState> s_PendingAwake =
            new List<BehaviourState>();
        private static readonly List<CoroutineState> s_Coroutines =
            new List<CoroutineState>();
        private static readonly List<Object> s_DeferredDestroy =
            new List<Object>();

        private static float s_Time;
        private static float s_FixedAccumulator;
        private static int s_FrameCount;

        // ---- native entry points ------------------------------------------

        // Deliberately empty: the M7 dispatch-cost measurement times
        // runtime-invoke vs direct methodPointer calls against this.
        internal static void Noop()
        {
        }

        // Wraps a native entity in a managed GameObject (idempotent per
        // handle) and instantiates the script on it. Failures are loud
        // console errors, never silent: a missing type means the link.xml or
        // the exporter is wrong, and the user must see that immediately.
        internal static void CreateScript(string typeName, int entityHandle)
        {
            GameObject go = GetOrCreateWrapper(entityHandle);
            if (go == null)
            {
                Debug.LogError("CreateScript: dead entity handle for '" + typeName + "'");
                return;
            }
            Type type = Type.GetType(typeName);
            if (type == null)
            {
                Debug.LogError("CreateScript: type not found: '" + typeName +
                               "' (is the game assembly preserved in link.xml?)");
                return;
            }
            if (!typeof(MonoBehaviour).IsAssignableFrom(type))
            {
                Debug.LogError("CreateScript: '" + typeName + "' is not a MonoBehaviour");
                return;
            }
            MonoBehaviour behaviour;
            try
            {
                behaviour = (MonoBehaviour)Activator.CreateInstance(type);
            }
            catch (Exception e)
            {
                Debug.LogError("CreateScript: constructing '" + typeName + "' threw: " + e);
                return;
            }
            behaviour.Attach(go);
            go.RegisterComponent(behaviour);
            Register(behaviour);
        }

        internal static void Register(MonoBehaviour behaviour)
        {
            BehaviourState state = Bind(behaviour);
            s_Behaviours.Add(state);
            s_PendingAwake.Add(state);
        }

        // One managed call per frame. Order per Unity's documented loop:
        // Awake/OnEnable/Start for newcomers, FixedUpdate catch-up, Update,
        // coroutine resume, LateUpdate, then end-of-frame destruction.
        internal static void Tick(float dt)
        {
            s_Time += dt;
            s_FrameCount += 1;
            Time.Sync(s_Time, dt, s_Time, Time.fixedDeltaTime, s_FrameCount);

            // Pads are polled before ANY script runs, so every callback in
            // this frame -- Awake included -- reads one consistent snapshot
            // and GetButtonDown fires in exactly one frame (M10 task 2).
            Native.ps2ur_input_update();

            // A scene load in flight advances by a bounded amount here, so
            // it keeps making progress whether or not a coroutine is
            // currently waiting on the AsyncOperation (M10 task 5).
            SceneManagement.SceneManager.Pump();

            DrainPending();

            s_FixedAccumulator += dt;
            while (s_FixedAccumulator >= Time.fixedDeltaTime)
            {
                s_FixedAccumulator -= Time.fixedDeltaTime;
                for (int i = 0; i < s_Behaviours.Count; i++)
                {
                    BehaviourState b = s_Behaviours[i];
                    if (b.FixedUpdate != null && IsRunnable(b))
                        b.FixedUpdate();
                }
                ResumeCoroutines(fixedStep: true);
            }

            for (int i = 0; i < s_Behaviours.Count; i++)
            {
                BehaviourState b = s_Behaviours[i];
                if (b.Update != null && IsRunnable(b))
                    b.Update();
            }

            ResumeCoroutines(fixedStep: false);

            // Animation lands HERE: after Update and coroutines, before
            // LateUpdate (M9 task 4). Scripts that read a bone-driven
            // transform in LateUpdate -- the standard camera-follow and
            // IK-fixup pattern -- see the posed skeleton, and scripts that
            // set animator parameters in Update have them applied the same
            // frame. Getting this order wrong is invisible until someone's
            // camera lags a frame behind the character.
            Native.ps2ur_anim_update(dt);

            for (int i = 0; i < s_Behaviours.Count; i++)
            {
                BehaviourState b = s_Behaviours[i];
                if (b.LateUpdate != null && IsRunnable(b))
                    b.LateUpdate();
            }

            ProcessDeferredDestroy();
        }

        // ---- object registry ----------------------------------------------

        internal static GameObject GetOrCreateWrapper(int entityHandle)
        {
            if (entityHandle == 0 || Native.ps2ur_entity_alive(entityHandle) == 0)
                return null;
            if (s_WrapperByHandle.TryGetValue(entityHandle, out GameObject existing))
                return existing;
            GameObject created = GameObject.FromExistingEntity(entityHandle);
            s_WrapperByHandle[entityHandle] = created;
            return created;
        }

        internal static void RegisterWrapper(GameObject go, int entityHandle)
        {
            s_WrapperByHandle[entityHandle] = go;
        }

        internal static void DeferDestroy(Object obj)
        {
            if (!s_DeferredDestroy.Contains(obj))
                s_DeferredDestroy.Add(obj);
        }

        // ---- coroutines (plan M7 task 5: managed IEnumerator pump) --------

        internal static Coroutine StartCoroutine(MonoBehaviour owner, IEnumerator routine)
        {
            CoroutineState state = new CoroutineState { Routine = routine, Owner = owner };
            // Unity runs a new coroutine synchronously up to its first yield.
            Advance(state);
            if (!state.Done)
                s_Coroutines.Add(state);
            return new Coroutine(state);
        }

        internal static void StopCoroutine(Coroutine coroutine)
        {
            if (coroutine != null && coroutine.m_State != null)
                coroutine.m_State.Done = true;
        }

        internal static void StopAllCoroutines(MonoBehaviour owner)
        {
            for (int i = 0; i < s_Coroutines.Count; i++)
            {
                if (ReferenceEquals(s_Coroutines[i].Owner, owner))
                    s_Coroutines[i].Done = true;
            }
        }

        private static void ResumeCoroutines(bool fixedStep)
        {
            for (int i = s_Coroutines.Count - 1; i >= 0; i--)
            {
                CoroutineState c = s_Coroutines[i];
                if (c.Done || c.Owner == null)
                {
                    s_Coroutines.RemoveAt(i);
                    continue;
                }
                if (fixedStep != c.WaitingForFixedUpdate)
                    continue;
                if (!fixedStep && c.SleepSeconds > 0f)
                {
                    c.SleepSeconds -= Time.deltaTime;
                    if (c.SleepSeconds > 0f)
                        continue;
                }
                if (!fixedStep && c.Routine.Current is CustomYieldInstruction wait &&
                    wait.keepWaiting)
                    continue;
                // yield return LoadSceneAsync(...) suspends until the load
                // finishes, as in the Editor. SceneManager.Pump advanced it
                // at the top of this frame.
                if (!fixedStep && c.Routine.Current is AsyncOperation op && !op.isDone)
                    continue;
                Advance(c);
                if (c.Done)
                    s_Coroutines.RemoveAt(i);
            }
        }

        private static void Advance(CoroutineState c)
        {
            c.SleepSeconds = 0f;
            c.WaitingForFixedUpdate = false;
            bool moved;
            try
            {
                moved = c.Routine.MoveNext();
            }
            catch (Exception e)
            {
                Debug.LogError("Coroutine threw: " + e);
                c.Done = true;
                return;
            }
            if (!moved)
            {
                c.Done = true;
                return;
            }
            object current = c.Routine.Current;
            if (current is WaitForSeconds seconds)
                c.SleepSeconds = seconds.m_Seconds;
            else if (current is WaitForFixedUpdate)
                c.WaitingForFixedUpdate = true;
            // null and CustomYieldInstruction need no state: null resumes
            // next frame, keepWaiting is polled in ResumeCoroutines.
        }

        // ---- internals ----------------------------------------------------

        private static void DrainPending()
        {
            // Unity's contract: every newcomer's Awake runs before any
            // newcomer's OnEnable, which runs before any Start.
            if (s_PendingAwake.Count == 0)
                return;
            List<BehaviourState> batch = new List<BehaviourState>(s_PendingAwake);
            s_PendingAwake.Clear();

            for (int i = 0; i < batch.Count; i++)
            {
                BehaviourState b = batch[i];
                if (!b.AwakeRan)
                {
                    b.AwakeRan = true;
                    b.Awake?.Invoke();
                }
            }
            for (int i = 0; i < batch.Count; i++)
            {
                BehaviourState b = batch[i];
                if (!b.EnabledRan && IsRunnable(b))
                {
                    b.EnabledRan = true;
                    b.OnEnable?.Invoke();
                }
            }
            for (int i = 0; i < batch.Count; i++)
            {
                BehaviourState b = batch[i];
                if (!b.StartRan && IsRunnable(b))
                {
                    b.StartRan = true;
                    b.Start?.Invoke();
                }
            }
        }

        private static bool IsRunnable(BehaviourState b)
        {
            MonoBehaviour mb = b.Behaviour;
            return mb != null && !mb.IsDestroyedInternal && mb.enabled &&
                   mb.gameObject != null && mb.gameObject.activeInHierarchy;
        }

        private static void ProcessDeferredDestroy()
        {
            if (s_DeferredDestroy.Count == 0)
                return;
            List<Object> batch = new List<Object>(s_DeferredDestroy);
            s_DeferredDestroy.Clear();
            for (int i = 0; i < batch.Count; i++)
            {
                if (batch[i] is GameObject go)
                    DestroyGameObjectNow(go);
                else if (batch[i] is MonoBehaviour mb)
                    DestroyBehaviourNow(mb);
                else
                    batch[i].MarkDestroyed();
            }
        }

        private static void DestroyGameObjectNow(GameObject go)
        {
            // OnDisable/OnDestroy for every behaviour in the subtree, then
            // one native destroy (which cascades) and managed marks.
            for (int i = 0; i < s_Behaviours.Count; i++)
            {
                BehaviourState b = s_Behaviours[i];
                if (b.Behaviour != null && !b.Behaviour.IsDestroyedInternal &&
                    IsInSubtree(b.Behaviour.gameObject, go))
                {
                    if (b.EnabledRan)
                        b.OnDisable?.Invoke();
                    b.OnDestroy?.Invoke();
                    b.Behaviour.MarkDestroyed();
                }
            }
            s_Behaviours.RemoveAll(b => b.Behaviour == null || b.Behaviour.IsDestroyedInternal);
            Native.ps2ur_entity_destroy(go.Handle);
            go.MarkDestroyed();
        }

        private static void DestroyBehaviourNow(MonoBehaviour mb)
        {
            for (int i = 0; i < s_Behaviours.Count; i++)
            {
                BehaviourState b = s_Behaviours[i];
                if (ReferenceEquals(b.Behaviour, mb))
                {
                    if (b.EnabledRan)
                        b.OnDisable?.Invoke();
                    b.OnDestroy?.Invoke();
                    break;
                }
            }
            s_Behaviours.RemoveAll(b => ReferenceEquals(b.Behaviour, mb));
            StopAllCoroutines(mb);
            mb.MarkDestroyed();
        }

        private static bool IsInSubtree(GameObject candidate, GameObject root)
        {
            Transform t = candidate != null ? candidate.transform : null;
            Transform rootT = root.transform;
            while (t != null)
            {
                if (ReferenceEquals(t, rootT) || t.Handle == rootT.Handle)
                    return true;
                t = t.parent;
            }
            return false;
        }

        private static BehaviourState Bind(MonoBehaviour behaviour)
        {
            Type type = behaviour.GetType();
            if (!s_TypeCache.TryGetValue(type, out TypeMethods methods))
            {
                methods = new TypeMethods
                {
                    Awake = FindMagicMethod(type, "Awake"),
                    OnEnable = FindMagicMethod(type, "OnEnable"),
                    Start = FindMagicMethod(type, "Start"),
                    Update = FindMagicMethod(type, "Update"),
                    FixedUpdate = FindMagicMethod(type, "FixedUpdate"),
                    LateUpdate = FindMagicMethod(type, "LateUpdate"),
                    OnDisable = FindMagicMethod(type, "OnDisable"),
                    OnDestroy = FindMagicMethod(type, "OnDestroy"),
                };
                s_TypeCache[type] = methods;
            }
            return new BehaviourState
            {
                Behaviour = behaviour,
                Awake = MakeAction(behaviour, methods.Awake),
                OnEnable = MakeAction(behaviour, methods.OnEnable),
                Start = MakeAction(behaviour, methods.Start),
                Update = MakeAction(behaviour, methods.Update),
                FixedUpdate = MakeAction(behaviour, methods.FixedUpdate),
                LateUpdate = MakeAction(behaviour, methods.LateUpdate),
                OnDisable = MakeAction(behaviour, methods.OnDisable),
                OnDestroy = MakeAction(behaviour, methods.OnDestroy),
            };
        }

        // Unity's magic methods are matched by name in the DECLARING type
        // chain (a private Update in a base class still runs). Zero-arg only.
        private static MethodInfo FindMagicMethod(Type type, string name)
        {
            for (Type t = type; t != null && t != typeof(MonoBehaviour); t = t.BaseType)
            {
                MethodInfo mi = t.GetMethod(
                    name,
                    BindingFlags.Instance | BindingFlags.Public |
                    BindingFlags.NonPublic | BindingFlags.DeclaredOnly,
                    null, System.Type.EmptyTypes, null);
                if (mi != null)
                    return mi;
            }
            return null;
        }

        private static Action MakeAction(MonoBehaviour target, MethodInfo method)
        {
            if (method == null)
                return null;
            return (Action)Delegate.CreateDelegate(typeof(Action), target, method);
        }
    }
}
