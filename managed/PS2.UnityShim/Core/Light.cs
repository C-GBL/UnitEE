using UnityEngine.Internal;

namespace UnityEngine
{
    // Unity's values, so a script's switch on LightType compiles unchanged.
    public enum LightType
    {
        Spot = 0,
        Directional = 1,
        Point = 2,
        Area = 3,
    }

    /// <summary>
    /// A light (M14). The runtime lights every object with the three
    /// brightest lights that reach it, per frame: directional lights along
    /// their transform's forward, point and spot lights as a directional
    /// light aimed at the object and attenuated by distance (deviation 39).
    /// Direction and position follow the transform, so moving the light
    /// moves the lighting. Area lights export as nothing.
    /// </summary>
    public sealed class Light : Behaviour
    {
        private LightType m_Type = LightType.Directional;
        private Color m_Color = Color.white;
        private float m_Intensity = 1f;
        private float m_Range = 10f;
        private float m_SpotAngle = 30f;
        private bool m_Enabled = true;

        private int Handle => gameObject != null ? gameObject.Handle : 0;

        public LightType type
        {
            get => m_Type;
            set { m_Type = value; Push(); }
        }

        public Color color
        {
            get => m_Color;
            set { m_Color = value; Push(); }
        }

        public float intensity
        {
            get => m_Intensity;
            set { m_Intensity = value; Push(); }
        }

        public float range
        {
            get => m_Range;
            set { m_Range = value; Push(); }
        }

        public float spotAngle
        {
            get => m_SpotAngle;
            set { m_SpotAngle = value; Push(); }
        }

        public new bool enabled
        {
            get => m_Enabled;
            set { m_Enabled = value; base.enabled = value; Push(); }
        }

        internal void InitFromNative(int kind, float r, float g, float b, float range,
                                     float spotCos, bool enabled)
        {
            m_Type = kind == 1 ? LightType.Point : kind == 2 ? LightType.Spot
                                                              : LightType.Directional;
            // The exporter premultiplied colour by intensity; keep intensity
            // 1 so the product round-trips.
            m_Color = new Color(r, g, b, 1f);
            m_Intensity = 1f;
            m_Range = range;
            m_SpotAngle = 2f * (float)System.Math.Acos(Mathf.Clamp(spotCos, -1f, 1f)) * Mathf.Rad2Deg;
            m_Enabled = enabled;
            base.enabled = enabled;
        }

        // A Light created by AddComponent reaches the runtime on its first
        // property write (there is no attach hook to bind on).
        private void Push()
        {
            if (Handle == 0)
                return;
            int kind = m_Type == LightType.Point ? 1 : m_Type == LightType.Spot ? 2 : 0;
            float spotCos = Mathf.Cos(m_SpotAngle * 0.5f * Mathf.Deg2Rad);
            Native.ps2ur_light_set(Handle, kind, m_Color.r * m_Intensity,
                                   m_Color.g * m_Intensity, m_Color.b * m_Intensity,
                                   m_Range, spotCos,
                                   m_Enabled && m_Type != LightType.Area ? 1 : 0);
        }
    }

    /// <summary>The scene-wide render settings the console models: ambient light.</summary>
    public static class RenderSettings
    {
        private static Color s_Ambient = new Color(0.157f, 0.157f, 0.157f, 1f);

        public static Color ambientLight
        {
            get => s_Ambient;
            set
            {
                s_Ambient = value;
                Native.ps2ur_scene_set_ambient(value.r, value.g, value.b);
            }
        }
    }
}
