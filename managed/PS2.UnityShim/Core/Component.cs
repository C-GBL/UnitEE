namespace UnityEngine
{
    public class Component : Object
    {
        private GameObject m_GameObject;

        public GameObject gameObject =>
            m_GameObject != null && !m_GameObject.IsDestroyedInternal ? m_GameObject : null;

        // The unfiltered owner, for internals that must work mid-destruction
        // (Transform needs its handle while OnDestroy runs).
        internal GameObject RawGameObject => m_GameObject;

        public Transform transform
        {
            get
            {
                GameObject go = m_GameObject;
                return go != null ? go.transform : null;
            }
        }

        internal override bool IsDestroyedInternal =>
            base.IsDestroyedInternal ||
            (m_GameObject != null && m_GameObject.IsDestroyedInternal);

        internal void Attach(GameObject go)
        {
            m_GameObject = go;
        }

        public T GetComponent<T>() where T : Component =>
            m_GameObject != null ? m_GameObject.GetComponent<T>() : null;
    }
}
