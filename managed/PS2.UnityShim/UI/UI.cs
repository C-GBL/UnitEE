using System;
using System.Collections.Generic;
using UnityEngine.Internal;

namespace UnityEngine.Events
{
    // The slice of UnityEvent that Button.onClick and Slider.onValueChanged
    // actually are in user code. Persistent (Inspector-serialised) listeners
    // do not exist on this platform -- deviation 31: wire listeners in code.
    public class UnityEvent
    {
        private readonly List<Action> m_Listeners = new List<Action>();
        public void AddListener(Action call) { if (call != null) m_Listeners.Add(call); }
        public void RemoveListener(Action call) { m_Listeners.Remove(call); }
        public void RemoveAllListeners() { m_Listeners.Clear(); }
        public void Invoke()
        {
            for (int i = 0; i < m_Listeners.Count; i++) m_Listeners[i]();
        }
    }

    public class UnityEvent<T>
    {
        private readonly List<Action<T>> m_Listeners = new List<Action<T>>();
        public void AddListener(Action<T> call) { if (call != null) m_Listeners.Add(call); }
        public void RemoveListener(Action<T> call) { m_Listeners.Remove(call); }
        public void RemoveAllListeners() { m_Listeners.Clear(); }
        public void Invoke(T value)
        {
            for (int i = 0; i < m_Listeners.Count; i++) m_Listeners[i](value);
        }
    }
}

namespace UnityEngine.UI
{
    // The uGUI subset (M12.5 task 5). Layout is baked at export: these
    // classes are live views over the native draw table -- colour, text,
    // visibility and pixel rects mutate; anchors do not exist at runtime
    // (deviation 30).
    //
    // A base for everything drawable, holding the element index and the
    // tint. Unity's Graphic is abstract here too.
    public abstract class Graphic : Behaviour
    {
        internal int Element = -1;
        private Color m_Color = Color.white;

        public Color color
        {
            get => m_Color;
            set
            {
                m_Color = value;
                Push();
            }
        }

        internal void Push()
        {
            if (Element >= 0)
            {
                Native.ps2ur_ui_set_colour(Element, Pack(m_Color));
            }
        }

        // RGBA8 with alpha mapped to the PS2's 0..0x80 opaque range.
        internal static uint Pack(Color c)
        {
            uint r = (uint)Mathf.Clamp(Mathf.RoundToInt(c.r * 255f), 0, 255);
            uint g = (uint)Mathf.Clamp(Mathf.RoundToInt(c.g * 255f), 0, 255);
            uint b = (uint)Mathf.Clamp(Mathf.RoundToInt(c.b * 255f), 0, 255);
            uint a = (uint)Mathf.Clamp(Mathf.RoundToInt(c.a * 128f), 0, 128);
            return r | (g << 8) | (b << 16) | (a << 24);
        }
    }

    public sealed class Image : Graphic
    {
    }

    public sealed class RawImage : Graphic
    {
    }

    public sealed class Text : Graphic
    {
        private string m_Text = "";

        public string text
        {
            get => m_Text;
            set
            {
                m_Text = value ?? "";
                if (Element >= 0)
                {
                    // 48-byte native cap, baked 8x8 font (deviation 30).
                    Native.ps2ur_ui_set_text(Element, m_Text);
                }
            }
        }
    }

    // Focusable controls. There is no pointer on a DualShock 2, so focus
    // moves with the D-pad through PS2UINavigation -- the honest replacement
    // for EventSystem (deviation 31).
    public abstract class Selectable : Behaviour
    {
        public bool interactable = true;
        internal Graphic Target; // the graphic the focus highlight tints
        private Color m_BaseColor = Color.white;
        private bool m_Focused;

        internal void SetTarget(Graphic target)
        {
            Target = target;
            if (target != null)
            {
                m_BaseColor = target.color;
            }
        }

        internal void SetFocused(bool focused)
        {
            if (m_Focused == focused || Target == null)
            {
                return;
            }
            m_Focused = focused;
            // Unity's ColorBlock reduced to its visible essence: highlighted
            // is a lightened tint of the authored colour.
            Target.color = focused
                ? Color.Lerp(m_BaseColor, Color.white, 0.5f)
                : m_BaseColor;
        }

        internal abstract void Submit();
        internal virtual void Adjust(int direction) { }
    }

    public sealed class Button : Selectable
    {
        public UnityEngine.Events.UnityEvent onClick =
            new UnityEngine.Events.UnityEvent();

        internal override void Submit()
        {
            if (interactable)
            {
                onClick.Invoke();
            }
        }
    }

    public sealed class Slider : Selectable
    {
        public UnityEngine.Events.UnityEvent<float> onValueChanged =
            new UnityEngine.Events.UnityEvent<float>();

        public float minValue = 0f;
        public float maxValue = 1f;

        private float m_Value;
        internal int FillElement = -1;
        internal float FillX, FillY, FillMaxW, FillH; // authored fill rect

        public float value
        {
            get => m_Value;
            set
            {
                float clamped = Mathf.Clamp(value, minValue, maxValue);
                if (clamped == m_Value)
                {
                    return;
                }
                m_Value = clamped;
                if (FillElement >= 0)
                {
                    float t = maxValue > minValue
                        ? (m_Value - minValue) / (maxValue - minValue)
                        : 0f;
                    Native.ps2ur_ui_set_rect(FillElement, FillX, FillY,
                                             FillMaxW * t, FillH);
                }
                onValueChanged.Invoke(m_Value);
            }
        }

        internal void Bootstrap(float initial)
        {
            m_Value = Mathf.Clamp(initial, minValue, maxValue);
        }

        internal override void Submit() { }

        internal override void Adjust(int direction)
        {
            if (interactable)
            {
                // A tenth of the range per tap: the d-pad is a discrete
                // instrument, and ten steps is what PS2-era options menus
                // actually did.
                value = m_Value + direction * (maxValue - minValue) * 0.1f;
            }
        }
    }
}

namespace UnityEngine
{
    // Pad-driven focus, the EventSystem replacement (M12.5 task 5,
    // deviation 31): Up/Down move focus in hierarchy order, Cross submits
    // the focused Button, Left/Right adjust the focused Slider. Registered
    // at scene load in export order, which IS hierarchy order.
    public static class PS2UINavigation
    {
        private static readonly List<UI.Selectable> s_Selectables =
            new List<UI.Selectable>();
        private static int s_Focus = -1;

        public static int selectableCount => s_Selectables.Count;

        internal static void Register(UI.Selectable selectable)
        {
            s_Selectables.Add(selectable);
            if (s_Focus < 0)
            {
                s_Focus = 0;
                selectable.SetFocused(true);
            }
        }

        internal static void Clear()
        {
            s_Selectables.Clear();
            s_Focus = -1;
        }

        internal static void Update()
        {
            if (s_Selectables.Count == 0)
            {
                return;
            }
            if (PS2Input.GetButtonDown(0, PS2Button.Down))
            {
                Move(1);
            }
            else if (PS2Input.GetButtonDown(0, PS2Button.Up))
            {
                Move(-1);
            }
            else if (PS2Input.GetButtonDown(0, PS2Button.Cross))
            {
                s_Selectables[s_Focus].Submit();
            }
            else if (PS2Input.GetButtonDown(0, PS2Button.Right))
            {
                s_Selectables[s_Focus].Adjust(1);
            }
            else if (PS2Input.GetButtonDown(0, PS2Button.Left))
            {
                s_Selectables[s_Focus].Adjust(-1);
            }
        }

        private static void Move(int direction)
        {
            if (s_Selectables.Count < 2)
            {
                return;
            }
            s_Selectables[s_Focus].SetFocused(false);
            s_Focus = (s_Focus + direction + s_Selectables.Count) %
                      s_Selectables.Count;
            s_Selectables[s_Focus].SetFocused(true);
        }
    }
}
