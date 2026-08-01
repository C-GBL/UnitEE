namespace UnityEngine
{
    public class Behaviour : Component
    {
        // TODO(native-backing): no OnEnable/OnDisable dispatch yet; the
        // runtime scheduler consumes this flag when it exists.
        private bool m_Enabled = true;

        public bool enabled
        {
            get => m_Enabled;
            set => m_Enabled = value;
        }

        public bool isActiveAndEnabled => m_Enabled && gameObject != null && gameObject.activeInHierarchy;
    }
}
