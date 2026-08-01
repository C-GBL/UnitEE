namespace PS2.UnityShim.Editor
{
    // Placeholder. This assembly is listed in the plan's repository layout
    // (section 8) but its contents are defined by the missing managed-shim
    // spec. TODO(spec missing: section 12).
    //
    // The one thing known to be needed already: the Editor play-mode stand-in
    // must flip the shim into console logging before any UnityEngine.Debug
    // call, otherwise Debug tries the __Internal P/Invoke (see
    // PS2.UnityShim/Internal/Native.cs).
    public static class ShimEditorPlaceholder
    {
        public static void EnableEditorLogging()
        {
            UnityEngine.Internal.ShimConfig.UseConsoleLog = true;
        }
    }
}
