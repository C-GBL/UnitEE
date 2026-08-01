using UnityEngine;

namespace Ps2.Runtime
{
    /// <summary>
    /// Placeholder for the editor-play-mode stand-in components (plan section 8,
    /// unity-package Runtime note). Real stand-ins arrive with the PS2.UnityShim
    /// integration so that behaviour in Editor play mode matches on-target
    /// behaviour for the supported API subset (plan section 7.1, criterion D5).
    /// TODO(spec missing: section 12): the managed shim spec defines which
    /// stand-ins exist and how they mirror the shim surface.
    /// </summary>
    [AddComponentMenu("")] // Hidden from the Add Component menu until real stand-ins exist.
    public sealed class PS2RuntimeStandIn : MonoBehaviour
    {
    }
}
