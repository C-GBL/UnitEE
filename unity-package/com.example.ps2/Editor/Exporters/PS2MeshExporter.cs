using UnityEngine;

namespace Ps2.Editor
{
    /// <summary>
    /// Converts a Unity Mesh into VU1-sized batches (plan section 3.2: realistically
    /// 64-96 vertices per batch with position + normal + UV + colour, double-buffered
    /// VU1 data memory) via the tri-stripper and VU1 batcher converters of the plan
    /// section 6 diagram.
    /// </summary>
    public static class PS2MeshExporter
    {
        /// <summary>Exports one mesh to a mesh-batch blob inside &lt;outDir&gt;.</summary>
        public static void ExportMesh(Mesh mesh, string outDir)
        {
            throw new PS2BuildException(
                "PS2MeshExporter not implemented; blocked on the p2b container spec (plan section 10, " +
                "missing). TODO(spec missing: section 10). Mesh: '" +
                (mesh != null ? mesh.name : "<null>") + "', outDir: '" + outDir + "'.");
        }
    }
}
