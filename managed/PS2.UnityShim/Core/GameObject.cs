using System;
using System.Collections.Generic;

namespace UnityEngine
{
    // Compiling skeleton of the entity type. Component storage is a plain
    // managed list for now so the Editor play-mode stand-in can reuse it.
    // TODO(native-backing): on target, creation/lookup route through ps2ur's
    // entity table via Internal.Native, and lifecycle events (Awake/OnEnable)
    // are dispatched by the runtime scheduler - none of that exists here yet.
    public sealed class GameObject : Object
    {
        private readonly List<Component> m_Components = new List<Component>();
        private readonly Transform m_Transform;
        private bool m_ActiveSelf = true;

        // TODO(native-backing): tags/layers become interned ids in the p2b
        // scene data. TODO(spec missing: section 10).
        public string tag = "Untagged";
        public int layer;

        public GameObject() : this("New Game Object") { }

        public GameObject(string name)
        {
            this.name = name;
            m_Transform = new Transform();
            m_Transform.Attach(this);
            m_Components.Add(m_Transform);
        }

        public Transform transform => m_Transform;

        public bool activeSelf => m_ActiveSelf;

        public bool activeInHierarchy
        {
            get
            {
                if (!m_ActiveSelf) return false;
                Transform p = m_Transform.parent;
                return p == null || p.gameObject.activeInHierarchy;
            }
        }

        // TODO(native-backing): no OnEnable/OnDisable dispatch yet.
        public void SetActive(bool value) => m_ActiveSelf = value;

        public T AddComponent<T>() where T : Component
        {
            // TODO(native-backing): no Awake/OnEnable dispatch yet; native
            // component registration missing.
            T c = Activator.CreateInstance<T>();
            c.Attach(this);
            m_Components.Add(c);
            return c;
        }

        public T GetComponent<T>() where T : Component
        {
            for (int i = 0; i < m_Components.Count; i++)
            {
                if (m_Components[i] is T t) return t;
            }
            return null;
        }

        public Component GetComponent(Type type)
        {
            for (int i = 0; i < m_Components.Count; i++)
            {
                if (type.IsInstanceOfType(m_Components[i])) return m_Components[i];
            }
            return null;
        }

        public T[] GetComponents<T>() where T : Component
        {
            List<T> result = new List<T>();
            for (int i = 0; i < m_Components.Count; i++)
            {
                if (m_Components[i] is T t) result.Add(t);
            }
            return result.ToArray();
        }

        public bool CompareTag(string tag) => this.tag == tag;

        public static GameObject Find(string name)
        {
            throw new NotImplementedException("TODO(native-backing): GameObject.Find requires the ps2ur scene registry.");
        }

        public static GameObject FindWithTag(string tag)
        {
            throw new NotImplementedException("TODO(native-backing): GameObject.FindWithTag requires the ps2ur scene registry.");
        }
    }
}
