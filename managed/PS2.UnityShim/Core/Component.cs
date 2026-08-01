namespace UnityEngine
{
    public class Component : Object
    {
        private GameObject m_GameObject;

        public GameObject gameObject => m_GameObject;
        public Transform transform => m_GameObject != null ? m_GameObject.transform : null;

        // TODO(native-backing): attachment will mirror the native entity table.
        internal void Attach(GameObject go)
        {
            m_GameObject = go;
        }

        public T GetComponent<T>() where T : Component =>
            m_GameObject != null ? m_GameObject.GetComponent<T>() : null;
    }
}
