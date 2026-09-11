using UnityEngine.Internal;

namespace UnityEngine
{
    // The runtime's on-screen diagnostics pages, in the native layer's
    // order (prof::Page). Select on the pad cycles through them.
    public enum PS2DebugPage
    {
        Off = 0,
        Frame,  // frame time, vsync budget, DMA
        Zones,  // profiler zones
        Memory, // memory regions: used / peak / capacity
        Vram,   // every VRAM allocation by name, and the occupancy map
    }

    // Console-only, in the spirit of PS2Input: the overlay is drawn by the
    // runtime in its own font, independent of the scene's UI, so it still
    // shows when the scene's own textures or fonts are the thing that is
    // broken. The page is runtime state and survives scene loads.
    public static class PS2DebugOverlay
    {
        public static PS2DebugPage Page
        {
            get { return (PS2DebugPage)Native.ps2ur_debug_overlay_page(); }
            set { Native.ps2ur_debug_overlay_set_page((int)value); }
        }

        public static void Show(PS2DebugPage page) { Page = page; }

        public static void Hide() { Page = PS2DebugPage.Off; }

        /// <summary>The next page, wrapping through Off, exactly as Select does.</summary>
        public static void Next()
        {
            int next = ((int)Page + 1) % ((int)PS2DebugPage.Vram + 1);
            Page = (PS2DebugPage)next;
        }
    }
}
