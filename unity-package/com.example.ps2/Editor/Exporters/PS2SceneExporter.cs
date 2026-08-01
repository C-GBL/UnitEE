using UnityEditor;

namespace Ps2.Editor
{
    /// <summary>
    /// Exports a Unity scene (entities, transform hierarchy, component data) to a
    /// scene.p2b container consumed by the ps2ur runtime (plan section 6 diagram).
    /// </summary>
    public static class PS2SceneExporter
    {
        /// <summary>Exports one scene to &lt;outDir&gt;/&lt;sceneName&gt;.p2b.</summary>
        public static void ExportScene(SceneAsset scene, string outDir)
        {
            throw new PS2BuildException(
                "PS2SceneExporter not implemented; blocked on the p2b container spec (plan section 10, " +
                "missing). TODO(spec missing: section 10). Scene: '" +
                (scene != null ? scene.name : "<null>") + "', outDir: '" + outDir + "'.");
        }
    }
}
