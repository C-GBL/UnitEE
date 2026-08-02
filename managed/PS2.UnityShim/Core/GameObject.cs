using System;
using System.Collections.Generic;
using UnityEngine.Internal;

namespace UnityEngine
{
    // The entity type, native-backed (plan section 12.2): a GameObject IS an
    // {index, generation} handle into ps2ur's entity table; active state and
    // the transform live natively, component instances live managed.
    public sealed class GameObject : Object
    {
        private readonly List<Component> m_Components = new List<Component>();
        private readonly Transform m_Transform;
        internal readonly int m_Handle;

        // TODO(p2b tags/layers): tags and layers become interned ids when the
        // exporter carries them; only the fields exist so far.
        public string tag = "Untagged";
        public int layer;

        public GameObject() : this("New Game Object") { }

        public GameObject(string name) : this(Native.ps2ur_entity_create(0), name)
        {
            if (m_Handle == 0)
                throw new InvalidOperationException(
                    "GameObject: native entity table is full (kMaxEntities).");
            Runtime.RegisterWrapper(this, m_Handle);
        }

        private GameObject(int handle, string name)
        {
            this.name = name;
            m_Handle = handle;
            m_Transform = new Transform(this);
            m_Components.Add(m_Transform);
        }

        // Wraps an entity that already exists natively (scene-loaded).
        internal static GameObject FromExistingEntity(int handle)
        {
            return new GameObject(handle, "Entity");
        }

        internal int Handle => m_Handle;

        internal override bool IsDestroyedInternal =>
            base.IsDestroyedInternal || Native.ps2ur_entity_alive(m_Handle) == 0;

        public Transform transform => m_Transform;

        public bool activeSelf => Native.ps2ur_entity_get_active(m_Handle) != 0;

        public bool activeInHierarchy
        {
            get
            {
                if (!activeSelf) return false;
                Transform p = m_Transform.parent;
                return p == null || p.gameObject.activeInHierarchy;
            }
        }

        public void SetActive(bool value) =>
            Native.ps2ur_entity_set_active(m_Handle, value ? 1 : 0);

        public T AddComponent<T>() where T : Component
        {
            T c = Activator.CreateInstance<T>();
            c.Attach(this);
            m_Components.Add(c);
            if (c is MonoBehaviour behaviour)
                Runtime.Register(behaviour);
            return c;
        }

        internal void RegisterComponent(Component c)
        {
            m_Components.Add(c);
        }

        public T GetComponent<T>() where T : Component
        {
            for (int i = 0; i < m_Components.Count; i++)
            {
                if (m_Components[i] is T t && !m_Components[i].IsDestroyedInternal)
                    return t;
            }
            return null;
        }

        public Component GetComponent(Type type)
        {
            for (int i = 0; i < m_Components.Count; i++)
            {
                if (type.IsInstanceOfType(m_Components[i]) && !m_Components[i].IsDestroyedInternal)
                    return m_Components[i];
            }
            return null;
        }
    }
}
