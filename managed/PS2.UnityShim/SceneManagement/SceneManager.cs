using UnityEngine.Internal;

namespace UnityEngine.SceneManagement
{
    public enum LoadSceneMode
    {
        Single = 0,
        Additive = 1,
    }

    // Scene loading, synchronous and asynchronous (plan section 9, M10 task
    // 5). The native SceneLoader does the work; this is the Unity-shaped
    // face of it.
    //
    // SUBSET DISCIPLINE (ADR-007). What differs from the Editor, and why:
    //
    //   - Scenes are addressed by NAME only. Unity's build-index overload is
    //     absent because a PS2 build has no Build Settings scene list to
    //     index into; the name maps to "<name>.p2b" on the disc.
    //   - There is no UnloadSceneAsync. Additive loading appends into the
    //     running world and the container it parsed from stays resident
    //     (meshes point straight into it, zero copy), so unloading one scene
    //     out of a merged world is not something this runtime can honestly
    //     do. Load Single to get back to one scene.
    //   - No sceneLoaded/sceneUnloaded events. AsyncOperation.completed
    //     covers the same ground for the async path.
    //
    // A scene NAME, not a path: LoadSceneAsync("level2") reads "level2.p2b".
    public static class SceneManager
    {
        // How much of a scene to read per frame. 64 KB is roughly one CD
        // sector group and leaves the frame time for a loading screen; the
        // read is bounded, not blocking.
        private const int kBytesPerFrame = 64 * 1024;

        private static SceneLoadOperation s_Active;
        private static string s_ActiveSceneName;

        public static string GetActiveSceneName() => s_ActiveSceneName;

        // Returns null only if the load could not be STARTED (no scene
        // buffer bound, or a load already in flight). A load that starts and
        // then fails reports isDone with progress 1; check
        // LoadFailed afterwards.
        public static AsyncOperation LoadSceneAsync(string sceneName) =>
            LoadSceneAsync(sceneName, LoadSceneMode.Single);

        public static AsyncOperation LoadSceneAsync(string sceneName, LoadSceneMode mode)
        {
            SceneLoadOperation op = new SceneLoadOperation();
            if (string.IsNullOrEmpty(sceneName))
            {
                Debug.LogError("LoadSceneAsync: empty scene name");
                op.StartFailed = true;
                return op;
            }
            int additive = mode == LoadSceneMode.Additive ? 1 : 0;
            if (Native.ps2ur_scene_load_begin(FileFor(sceneName), additive) == 0)
            {
                Debug.LogError("LoadSceneAsync: could not start loading '" + sceneName + "'");
                op.StartFailed = true;
                return op;
            }
            s_Active = op;
            s_ActiveSceneName = sceneName;
            return op;
        }

        // Blocking load, for a boot scene where there is no frame to share.
        // Unity's LoadScene is itself synchronous-at-the-frame-boundary, so
        // the shape matches; the difference is that this one really does
        // stall until the disc is done.
        public static void LoadScene(string sceneName) =>
            LoadScene(sceneName, LoadSceneMode.Single);

        public static void LoadScene(string sceneName, LoadSceneMode mode)
        {
            AsyncOperation op = LoadSceneAsync(sceneName, mode);
            int guard = 0;
            while (!op.isDone && guard++ < 100000)
                Pump();
        }

        // True when the last load that STARTED ended in failure.
        public static bool LoadFailed =>
            Native.ps2ur_scene_load_state() == SceneLoadOperation.StateFailed;

        // A scene name becomes a container file name; the native side picks
        // the device it actually lives on (host:, the working directory, or
        // the disc). A name that already ends in .p2b is left alone so a
        // project can name its scenes either way.
        private static string FileFor(string sceneName)
        {
            return sceneName.EndsWith(".p2b") ? sceneName : sceneName + ".p2b";
        }

        // Called once per frame by the lifecycle driver. Advancing the load
        // here rather than inside the coroutine is what lets a script simply
        // `yield return op` -- and it means a load keeps progressing even if
        // nothing is waiting on it.
        internal static void Pump()
        {
            if (s_Active == null)
                return;
            Native.ps2ur_scene_load_update(kBytesPerFrame);
            if (!s_Active.isDone)
                return;
            SceneLoadOperation finished = s_Active;
            s_Active = null;
            // Say it in the managed log too: a failed load otherwise looks
            // exactly like a load nobody asked for -- the old scene keeps
            // running and nothing on screen changes.
            if (LoadFailed)
            {
                Debug.LogError("SceneManager: loading '" + s_ActiveSceneName +
                               "' failed; the previous scene is still active." +
                               " The native log has the reason.");
            }
            finished.RaiseCompleted();
        }
    }
}
