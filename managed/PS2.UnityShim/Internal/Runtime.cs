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
        internal sealed class TypeMethods
        {
            public MethodInfo Awake, OnEnable, Start, Update, FixedUpdate;
            public MethodInfo LateUpdate, OnDisable, OnDestroy;
            // Physics callbacks (M11 task 3). Kept as MethodInfo and invoked
            // with one argument, so they cannot be Action delegates like the
            // zero-arg lifecycle methods above.
            public MethodInfo OnCollisionEnter, OnCollisionStay, OnCollisionExit;
            public MethodInfo OnTriggerEnter, OnTriggerStay, OnTriggerExit;
        }

        internal sealed class BehaviourState
        {
            public MonoBehaviour Behaviour;
            public Action Awake, OnEnable, Start, Update, FixedUpdate;
            public Action LateUpdate, OnDisable, OnDestroy;
            public bool AwakeRan, StartRan, EnabledRan;
            public TypeMethods Methods;
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

        // Instantiates the Rigidbody a scene exported and binds it to the
        // collider the PHYS section put on the same entity. Called once per
        // Rigidbody at boot, BEFORE the first Tick -- so a script's Start()
        // doing GetComponent<Rigidbody>() finds one, which is the entire
        // reason the component travels in the container at all.
        internal static void CreateRigidbody(int entityHandle, float mass,
                                             float linearDamping,
                                             float angularDamping, int flags)
        {
            GameObject go = GetOrCreateWrapper(entityHandle);
            if (go == null)
            {
                Debug.LogError("CreateRigidbody: dead entity handle");
                return;
            }
            Rigidbody body = new Rigidbody();
            body.Attach(go);
            body.mass = mass;
            body.linearDamping = linearDamping;
            body.angularDamping = angularDamping;
            body.useGravity = (flags & 1) != 0;
            body.isKinematic = (flags & 2) != 0;
            body.freezeRotation = (flags & 4) != 0;
            // Registered before Bind so that even a body that fails to bind
            // is findable: GetComponent<Rigidbody>() returning null would
            // turn a diagnosable configuration problem into a
            // NullReferenceException in user code.
            go.RegisterComponent(body);
            body.Bind(Native.ps2ur_phys_collider_for_entity(entityHandle));
        }

        // Maps every native collider to the entity it rides on, so a contact
        // can be delivered to the right GameObject.
        //
        // ONE call for the whole table rather than one per collider, and it
        // stores HANDLES rather than GameObjects: a wrapper is a managed
        // allocation, and materialising one for all 500 colliders in a scene
        // would spend the GC heap on objects no script will ever look at.
        // DeliverContact resolves a handle to a wrapper only when a contact
        // actually involves it.
        internal static void BindColliders()
        {
            int count = Native.ps2ur_phys_collider_count();
            for (int i = 0; i < count; i++)
            {
                int handle = Native.ps2ur_phys_collider_entity(i);
                if (handle != 0)
                    s_HandleByCollider[i] = handle;
            }
        }

        // Instantiates the Animator a scene exported. The shim's Animator
        // addresses the native animator through its entity handle, so there
        // is no index to bind -- what this buys is GetComponent<Animator>()
        // returning something, which is the whole failure this phase exists
        // to fix (ADR-010).
        internal static void CreateAnimator(int entityHandle)
        {
            GameObject go = GetOrCreateWrapper(entityHandle);
            if (go == null)
            {
                Debug.LogError("CreateAnimator: dead entity handle");
                return;
            }
            Animator animator = new Animator();
            animator.Attach(go);
            go.RegisterComponent(animator);
        }

        // One call per AudioSource component at scene load (M12.5 task 2),
        // carrying the Editor-authored state. flags: bit0 playOnAwake,
        // bit1 loop, bit2 spatial; priority arrives on the NATIVE
        // higher-wins scale. playOnAwake fires here -- before the first
        // Update, which is when Unity starts such a source.
        internal static void CreateAudioSource(int entityHandle, int clip,
                                               float volume, int flags,
                                               float minDistance,
                                               float maxDistance, int priority)
        {
            GameObject go = GetOrCreateWrapper(entityHandle);
            if (go == null)
            {
                Debug.LogError("CreateAudioSource: dead entity handle");
                return;
            }
            var source = new AudioSource();
            source.Attach(go);
            source.Configure(clip >= 0 ? new AudioClip(clip) : null, volume,
                             (flags & 2) != 0, (flags & 4) != 0, minDistance,
                             maxDistance, priority);
            go.RegisterComponent(source);
            if ((flags & 1) != 0)
            {
                source.Play();
            }
        }

        internal static void CreatePS2ParticleSystem(int entityHandle)
        {
            GameObject go = GetOrCreateWrapper(entityHandle);
            if (go == null)
            {
                Debug.LogError("CreatePS2ParticleSystem: dead entity handle");
                return;
            }
            var system = new Ps2.Runtime.PS2ParticleSystem();
            system.Attach(go);
            go.RegisterComponent(system);
        }

        internal static void CreateAudioListener(int entityHandle)
        {
            GameObject go = GetOrCreateWrapper(entityHandle);
            if (go == null)
            {
                Debug.LogError("CreateAudioListener: dead entity handle");
                return;
            }
            var listener = new AudioListener();
            listener.Attach(go);
            go.RegisterComponent(listener);
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
                // Physics steps AFTER FixedUpdate, so a force added there
                // is integrated in the same step -- Unity's order, and the
                // one every physics tutorial assumes.
                Native.ps2ur_phys_step();
                // Then the callbacks it produced, before coroutines resume,
                // so a coroutine waiting on a trigger sees it this step.
                DispatchContacts();
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

        // ---- physics callbacks (M11 task 3) -------------------------------
        //
        // The whole contact list is pulled across with ONE call for the count
        // and one per contact, then fanned out to behaviours entirely in
        // managed code. Doing it the other way -- native calling into managed
        // per event -- would cost a runtime_invoke per contact, and M7
        // measured that at 87x a direct call.

        private static readonly Collision s_Collision = new Collision();

        private static void DispatchContacts()
        {
            int count = Native.ps2ur_phys_contact_count();
            if (count == 0)
                return;

            for (int i = 0; i < count; i++)
            {
                NativeContact nc = default;
                if (Native.ps2ur_phys_get_contact(i, ref nc) == 0)
                    continue;
                DeliverContact(nc, nc.colliderA, nc.colliderB);
                // Both sides get the callback, with the OTHER side reported
                // as 'other'. A contact against static geometry (colliderB
                // -1) only has one side to notify.
                if (nc.colliderB >= 0)
                    DeliverContact(nc, nc.colliderB, nc.colliderA);
            }
        }

        private static void DeliverContact(NativeContact nc, int self, int other)
        {
            GameObject go = GameObjectForCollider(self);
            if (go == null)
                return;

            // The Collision object is REUSED across callbacks. Unity
            // allocates a fresh one; a console with a 4 MB managed heap and
            // a 2 ms GC budget cannot afford one allocation per contact per
            // frame. Documented in supported-api.md: do not cache it.
            s_Collision.point = new Vector3(nc.pointX, nc.pointY, nc.pointZ);
            s_Collision.normal = new Vector3(nc.normalX, nc.normalY, nc.normalZ);
            s_Collision.separation = nc.separation;
            s_Collision.otherColliderIndex = other;
            s_Collision.gameObject = GameObjectForCollider(other);

            bool isTrigger = nc.isTrigger != 0;
            for (int i = 0; i < s_Behaviours.Count; i++)
            {
                BehaviourState b = s_Behaviours[i];
                if (b.Methods == null || !IsRunnable(b))
                    continue;
                if (!ReferenceEquals(b.Behaviour.gameObject, go))
                    continue;

                MethodInfo method = null;
                if (isTrigger)
                {
                    // 0 Enter, 1 Stay, 2 Exit -- ps2ur::phys::ContactPhase.
                    method = nc.phase == 0 ? b.Methods.OnTriggerEnter
                           : nc.phase == 1 ? b.Methods.OnTriggerStay
                                           : b.Methods.OnTriggerExit;
                }
                else
                {
                    method = nc.phase == 0 ? b.Methods.OnCollisionEnter
                           : nc.phase == 1 ? b.Methods.OnCollisionStay
                                           : b.Methods.OnCollisionExit;
                }
                if (method == null)
                    continue;
                try
                {
                    s_ContactArgs[0] = s_Collision;
                    method.Invoke(b.Behaviour, s_ContactArgs);
                }
                catch (Exception e)
                {
                    Debug.LogError("Physics callback threw: " + e);
                }
            }
        }

        private static readonly object[] s_ContactArgs = new object[1];
        private static readonly Dictionary<int, GameObject> s_GameObjectByCollider =
            new Dictionary<int, GameObject>();
        private static readonly Dictionary<int, int> s_HandleByCollider =
            new Dictionary<int, int>();

        internal static void RegisterCollider(int colliderIndex, GameObject go)
        {
            if (colliderIndex >= 0)
                s_GameObjectByCollider[colliderIndex] = go;
        }

        // A collider's GameObject: an explicit registration first (a wrapper
        // created by script or by AddComponent), then the entity handle
        // BindColliders recorded, resolved to a wrapper on first use.
        private static GameObject GameObjectForCollider(int colliderIndex)
        {
            if (colliderIndex < 0)
                return null;
            if (s_GameObjectByCollider.TryGetValue(colliderIndex, out GameObject found))
                return found;
            if (!s_HandleByCollider.TryGetValue(colliderIndex, out int handle))
                return null;
            GameObject go = GetOrCreateWrapper(handle);
            if (go != null)
                s_GameObjectByCollider[colliderIndex] = go;
            return go;
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
                    OnCollisionEnter = FindContactMethod(type, "OnCollisionEnter"),
                    OnCollisionStay = FindContactMethod(type, "OnCollisionStay"),
                    OnCollisionExit = FindContactMethod(type, "OnCollisionExit"),
                    OnTriggerEnter = FindContactMethod(type, "OnTriggerEnter"),
                    OnTriggerStay = FindContactMethod(type, "OnTriggerStay"),
                    OnTriggerExit = FindContactMethod(type, "OnTriggerExit"),
                };
                s_TypeCache[type] = methods;
            }
            return new BehaviourState
            {
                Behaviour = behaviour,
                Methods = methods,
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

        // The physics callbacks take one Collision argument. Unity's real
        // signatures are OnCollision*(Collision) and OnTrigger*(Collider);
        // both take Collision here, because a Collider in this runtime is an
        // index rather than a component and a trigger callback that handed
        // back a null Collider would be useless.
        private static MethodInfo FindContactMethod(Type type, string name)
        {
            Type[] signature = { typeof(Collision) };
            for (Type t = type; t != null && t != typeof(MonoBehaviour); t = t.BaseType)
            {
                MethodInfo mi = t.GetMethod(
                    name,
                    BindingFlags.Instance | BindingFlags.Public |
                    BindingFlags.NonPublic | BindingFlags.DeclaredOnly,
                    null, signature, null);
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
