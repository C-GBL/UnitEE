// Inspector and serialization attributes.
//
// These exist so that scripts written in the Editor compile unchanged against
// the shim. On the console every one of them is inert: there is no Inspector
// to draw a header or clamp a range, and the scene exporter (P2bSceneExporter,
// "User scripts") carries a MonoBehaviour across as a TYPE REFERENCE only.
// Field values come from the C# initialisers, never from what was typed into
// the Inspector -- [SerializeField] here changes nothing about that.
//
// Signatures match Unity's so that argument lists and named properties written
// against the real UnityEngine keep compiling. Only attributes with no runtime
// meaning are here on purpose. DefaultExecutionOrder and
// RuntimeInitializeOnLoadMethod are deliberately absent: the runtime does not
// honour them, and a silent no-op would be a worse surprise than a compile
// error naming them.

using System;

namespace UnityEngine
{
    [AttributeUsage(AttributeTargets.Field, Inherited = true, AllowMultiple = false)]
    public abstract class PropertyAttribute : Attribute
    {
        public int order { get; set; }
    }

    // --- Serialization markers ---------------------------------------------

    [AttributeUsage(AttributeTargets.Field, Inherited = true, AllowMultiple = false)]
    public sealed class SerializeField : Attribute { }

    [AttributeUsage(AttributeTargets.Field, Inherited = true, AllowMultiple = false)]
    public sealed class SerializeReference : Attribute { }

    [AttributeUsage(AttributeTargets.Field, Inherited = true, AllowMultiple = false)]
    public sealed class HideInInspector : Attribute { }

    [AttributeUsage(AttributeTargets.Field, Inherited = true, AllowMultiple = false)]
    public sealed class NonReorderableAttribute : Attribute { }

    // --- Inspector decoration (PropertyAttribute family) ---------------------

    public sealed class HeaderAttribute : PropertyAttribute
    {
        public readonly string header;
        public HeaderAttribute(string header) { this.header = header; }
    }

    public sealed class TooltipAttribute : PropertyAttribute
    {
        public readonly string tooltip;
        public TooltipAttribute(string tooltip) { this.tooltip = tooltip; }
    }

    public sealed class SpaceAttribute : PropertyAttribute
    {
        public readonly float height;
        public SpaceAttribute() { height = 8f; }
        public SpaceAttribute(float height) { this.height = height; }
    }

    public sealed class RangeAttribute : PropertyAttribute
    {
        public readonly float min;
        public readonly float max;
        public RangeAttribute(float min, float max) { this.min = min; this.max = max; }
    }

    public sealed class MinAttribute : PropertyAttribute
    {
        public readonly float min;
        public MinAttribute(float min) { this.min = min; }
    }

    public sealed class MultilineAttribute : PropertyAttribute
    {
        public readonly int lines;
        public MultilineAttribute() { lines = 3; }
        public MultilineAttribute(int lines) { this.lines = lines; }
    }

    public sealed class TextAreaAttribute : PropertyAttribute
    {
        public readonly int minLines;
        public readonly int maxLines;
        public TextAreaAttribute() { minLines = 3; maxLines = 3; }
        public TextAreaAttribute(int minLines, int maxLines) { this.minLines = minLines; this.maxLines = maxLines; }
    }

    public sealed class DelayedAttribute : PropertyAttribute { }

    public sealed class InspectorNameAttribute : PropertyAttribute
    {
        public readonly string displayName;
        public InspectorNameAttribute(string displayName) { this.displayName = displayName; }
    }

    public sealed class ColorUsageAttribute : PropertyAttribute
    {
        public readonly bool showAlpha = true;
        public readonly bool hdr = false;
        public ColorUsageAttribute(bool showAlpha) { this.showAlpha = showAlpha; }
        public ColorUsageAttribute(bool showAlpha, bool hdr) { this.showAlpha = showAlpha; this.hdr = hdr; }
    }

    public sealed class GradientUsageAttribute : PropertyAttribute
    {
        public readonly bool hdr;
        public GradientUsageAttribute(bool hdr) { this.hdr = hdr; }
    }

    public sealed class ContextMenuItemAttribute : PropertyAttribute
    {
        public readonly string name;
        public readonly string function;
        public ContextMenuItemAttribute(string name, string function) { this.name = name; this.function = function; }
    }

    // --- Class and method level ------------------------------------------------

    [AttributeUsage(AttributeTargets.Method, Inherited = true, AllowMultiple = false)]
    public sealed class ContextMenu : Attribute
    {
        public readonly string menuItem;
        public readonly bool validate;
        public readonly int priority;
        public ContextMenu(string itemName) : this(itemName, false, 1000000) { }
        public ContextMenu(string itemName, bool isValidateFunction) : this(itemName, isValidateFunction, 1000000) { }
        public ContextMenu(string itemName, bool isValidateFunction, int priority)
        {
            menuItem = itemName;
            validate = isValidateFunction;
            this.priority = priority;
        }
    }

    [AttributeUsage(AttributeTargets.Class, Inherited = true, AllowMultiple = true)]
    public sealed class RequireComponent : Attribute
    {
        public Type m_Type0;
        public Type m_Type1;
        public Type m_Type2;
        public RequireComponent(Type requiredComponent) { m_Type0 = requiredComponent; }
        public RequireComponent(Type requiredComponent, Type requiredComponent2)
        {
            m_Type0 = requiredComponent; m_Type1 = requiredComponent2;
        }
        public RequireComponent(Type requiredComponent, Type requiredComponent2, Type requiredComponent3)
        {
            m_Type0 = requiredComponent; m_Type1 = requiredComponent2; m_Type2 = requiredComponent3;
        }
    }

    [AttributeUsage(AttributeTargets.Class, Inherited = false, AllowMultiple = false)]
    public sealed class AddComponentMenu : Attribute
    {
        public string componentMenu;
        public int componentOrder = int.MaxValue;
        public AddComponentMenu(string menuName) { componentMenu = menuName; }
        public AddComponentMenu(string menuName, int order) { componentMenu = menuName; componentOrder = order; }
    }

    [AttributeUsage(AttributeTargets.Class, Inherited = false, AllowMultiple = false)]
    public sealed class DisallowMultipleComponent : Attribute { }

    [AttributeUsage(AttributeTargets.Class, Inherited = false, AllowMultiple = false)]
    public sealed class ExecuteInEditMode : Attribute { }

    [AttributeUsage(AttributeTargets.Class, Inherited = false, AllowMultiple = false)]
    public sealed class ExecuteAlways : Attribute { }

    [AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct | AttributeTargets.Enum |
                    AttributeTargets.Delegate | AttributeTargets.Interface,
                    Inherited = false, AllowMultiple = false)]
    public sealed class HelpURLAttribute : Attribute
    {
        public string URL;
        public HelpURLAttribute(string url) { URL = url; }
    }

    [AttributeUsage(AttributeTargets.Class, Inherited = false, AllowMultiple = false)]
    public sealed class SelectionBaseAttribute : Attribute { }

    [AttributeUsage(AttributeTargets.Class, Inherited = false, AllowMultiple = false)]
    public sealed class CreateAssetMenuAttribute : Attribute
    {
        public string menuName;
        public string fileName;
        public int order;
    }

    [AttributeUsage(AttributeTargets.Class, Inherited = false, AllowMultiple = false)]
    public sealed class PreferBinarySerialization : Attribute { }

    [AttributeUsage(AttributeTargets.Class | AttributeTargets.Assembly, Inherited = false, AllowMultiple = false)]
    public sealed class IconAttribute : Attribute
    {
        public string path;
        public IconAttribute(string path) { this.path = path; }
    }
}

namespace UnityEngine.Serialization
{
    [AttributeUsage(AttributeTargets.Field, Inherited = true, AllowMultiple = true)]
    public sealed class FormerlySerializedAsAttribute : Attribute
    {
        public string oldName;
        public FormerlySerializedAsAttribute(string oldName) { this.oldName = oldName; }
    }
}
