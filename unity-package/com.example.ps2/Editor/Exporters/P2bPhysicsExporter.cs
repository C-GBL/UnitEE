// Collider baking: Unity colliders -> the PHYS section (plan section 9,
// M11 task 1; format: docs/formats/p2b-container.md).
//
// Two halves, matching the runtime's static/dynamic split:
//
//   PRIMITIVES (Sphere/Box/Capsule) are exported as-is, in local space with
//   the entity's transform applied at runtime. That keeps them movable.
//
//   MESH COLLIDERS are baked to WORLD SPACE triangles and indexed by a BVH
//   built offline. World space is what makes the runtime traversal free of
//   per-query transforms, and it is why a mesh collider cannot move: baking
//   is the whole point.
//
// The BVH built here is the same algorithm the runtime's build_bvh uses,
// reimplemented in C# rather than P/Invoked. That is a deliberate
// duplication, the same one P2bWriter already makes against the C++ reader:
// both sides written from the spec means a disagreement shows up as a
// failing test rather than as collision that is subtly wrong.
using System;
using System.Collections.Generic;
using System.IO;
using UnityEngine;

namespace Ps2.Editor
{
    internal static class P2bPhysicsExporter
    {
        // Must match ps2ur::phys::kBvhLeafSize.
        private const int LeafSize = 4;
        private const int MaxDepth = 32;
        private const int MaxVertices = 65536;

        private struct Tri
        {
            public ushort V0, V1, V2;
            public byte Layer;
            public byte Flags;
        }

        private struct Node
        {
            public Vector3 Min, Max;
            public uint First;
            public uint Count;
        }

        internal struct ColliderRecord
        {
            public uint Kind;         // 0 sphere, 1 box, 2 capsule, 3 mesh
            public bool IsTrigger;
            public bool Enabled;
            public uint Axis;         // capsule direction
            public uint Layer;
            public int Entity;
            public Vector3 Center;
            public Vector3 HalfExtents;
            public float Height;
        }

        internal sealed class BakeResult
        {
            public byte[] Payload;
            public int ColliderCount;
            public int TriangleCount;
            public int NodeCount;
            public int VertexCount;
            public List<string> Warnings = new List<string>();
        }

        // 'entityOf' maps a GameObject to its exported entity index, or -1
        // for something that is not in the scene table.
        public static BakeResult Bake(IEnumerable<GameObject> roots,
                                      Func<GameObject, int> entityOf)
        {
            var result = new BakeResult();
            var colliders = new List<ColliderRecord>();
            var vertices = new List<Vector3>();
            var triangles = new List<Tri>();

            foreach (GameObject root in roots)
            {
                if (root == null)
                    continue;
                foreach (Collider c in root.GetComponentsInChildren<Collider>(true))
                {
                    int entity = entityOf != null ? entityOf(c.gameObject) : -1;
                    int layer = c.gameObject.layer & 31;

                    if (c is MeshCollider mesh)
                    {
                        AppendMeshCollider(mesh, layer, vertices, triangles, result);
                        continue;
                    }
                    ColliderRecord record;
                    if (TryPrimitive(c, entity, layer, out record, result))
                        colliders.Add(record);
                }
            }

            if (vertices.Count > MaxVertices)
            {
                result.Warnings.Add(
                    $"collision mesh has {vertices.Count} vertices; the format " +
                    $"indexes with u16 and caps at {MaxVertices}. Reduce the " +
                    "collision geometry -- it does not have to match the render mesh.");
                return result;
            }

            var triArray = triangles.ToArray();
            var nodes = new Node[Math.Max(1, triArray.Length * 2 + 1)];
            int nodeCount = BuildBvh(triArray, vertices, nodes);

            result.Payload = Serialize(colliders, nodes, nodeCount, triArray,
                                       vertices);
            result.ColliderCount = colliders.Count;
            result.TriangleCount = triArray.Length;
            result.NodeCount = nodeCount;
            result.VertexCount = vertices.Count;
            return result;
        }

        private static bool TryPrimitive(Collider c, int entity, int layer,
                                         out ColliderRecord record,
                                         BakeResult result)
        {
            record = default;
            record.IsTrigger = c.isTrigger;
            record.Enabled = c.enabled;
            record.Entity = entity;
            record.Layer = (uint)layer;

            // Non-uniform scale is the trap here. Unity bakes scale into a
            // collider's effective size using the LARGEST axis for spheres
            // and capsules, which means a squashed sphere collides as a
            // sphere. Rather than silently reproduce that surprise, the
            // exporter warns: the author almost never meant it.
            Vector3 scale = c.transform.lossyScale;
            bool uniform = Mathf.Abs(scale.x - scale.y) < 1e-3f &&
                           Mathf.Abs(scale.y - scale.z) < 1e-3f;

            if (c is SphereCollider sphere)
            {
                record.Kind = 0;
                float radius = sphere.radius * MaxAbs(scale);
                record.Center = sphere.center;
                record.HalfExtents = new Vector3(radius, radius, radius);
                if (!uniform)
                    result.Warnings.Add(
                        $"{Path(c)}: SphereCollider under non-uniform scale " +
                        $"{scale}; the radius uses the largest axis, so it will " +
                        "not match the visual shape.");
                return true;
            }
            if (c is BoxCollider box)
            {
                record.Kind = 1;
                record.Center = box.center;
                record.HalfExtents = new Vector3(
                    box.size.x * 0.5f * Mathf.Abs(scale.x),
                    box.size.y * 0.5f * Mathf.Abs(scale.y),
                    box.size.z * 0.5f * Mathf.Abs(scale.z));
                return true;
            }
            if (c is CapsuleCollider capsule)
            {
                record.Kind = 2;
                record.Axis = (uint)capsule.direction;
                record.Center = capsule.center;
                // Unity scales a capsule's radius by the two axes it is NOT
                // aligned with, and its height by the axis it is.
                float radiusScale, heightScale;
                switch (capsule.direction)
                {
                    case 0:
                        radiusScale = Mathf.Max(Mathf.Abs(scale.y), Mathf.Abs(scale.z));
                        heightScale = Mathf.Abs(scale.x);
                        break;
                    case 2:
                        radiusScale = Mathf.Max(Mathf.Abs(scale.x), Mathf.Abs(scale.y));
                        heightScale = Mathf.Abs(scale.z);
                        break;
                    default:
                        radiusScale = Mathf.Max(Mathf.Abs(scale.x), Mathf.Abs(scale.z));
                        heightScale = Mathf.Abs(scale.y);
                        break;
                }
                float r = capsule.radius * radiusScale;
                record.HalfExtents = new Vector3(r, r, r);
                record.Height = capsule.height * heightScale;
                if (record.Height < 2f * r)
                {
                    // Unity degenerates this to a sphere and so does the
                    // runtime; say so rather than let it surprise someone.
                    result.Warnings.Add(
                        $"{Path(c)}: capsule height {record.Height:0.###} is less " +
                        $"than its diameter {(2f * r):0.###}; it behaves as a sphere.");
                }
                return true;
            }

            result.Warnings.Add(
                $"{Path(c)}: {c.GetType().Name} is not supported and was skipped. " +
                "Supported: SphereCollider, BoxCollider, CapsuleCollider, MeshCollider.");
            return false;
        }

        private static void AppendMeshCollider(MeshCollider mc, int layer,
                                               List<Vector3> vertices,
                                               List<Tri> triangles,
                                               BakeResult result)
        {
            Mesh mesh = mc.sharedMesh;
            if (mesh == null)
            {
                result.Warnings.Add($"{Path(mc)}: MeshCollider with no mesh, skipped.");
                return;
            }
            if (!mesh.isReadable)
            {
                result.Warnings.Add(
                    $"{Path(mc)}: mesh '{mesh.name}' is not readable. Enable " +
                    "Read/Write in the model importer or it cannot be baked.");
                return;
            }
            // A convex MeshCollider in Unity is a hull used for a DYNAMIC
            // body. Baking it as static triangles would silently change what
            // it does, so it is refused rather than quietly reinterpreted.
            if (mc.convex)
            {
                result.Warnings.Add(
                    $"{Path(mc)}: convex MeshCollider is not supported (baked " +
                    "collision is static). Use a primitive collider for moving " +
                    "objects.");
                return;
            }

            Matrix4x4 toWorld = mc.transform.localToWorldMatrix;
            Vector3[] src = mesh.vertices;
            int[] idx = mesh.triangles;
            int baseIndex = vertices.Count;

            for (int i = 0; i < src.Length; i++)
                vertices.Add(toWorld.MultiplyPoint3x4(src[i]));

            for (int i = 0; i + 2 < idx.Length; i += 3)
            {
                int a = baseIndex + idx[i];
                int b = baseIndex + idx[i + 1];
                int c = baseIndex + idx[i + 2];
                if (a > ushort.MaxValue || b > ushort.MaxValue || c > ushort.MaxValue)
                    return; // the vertex cap is reported by the caller
                // Degenerate triangles have no normal and no useful contact;
                // they are also what makes a BVH split badly.
                Vector3 e1 = vertices[b] - vertices[a];
                Vector3 e2 = vertices[c] - vertices[a];
                if (Vector3.Cross(e1, e2).sqrMagnitude < 1e-12f)
                    continue;
                triangles.Add(new Tri
                {
                    V0 = (ushort)a,
                    V1 = (ushort)b,
                    V2 = (ushort)c,
                    Layer = (byte)layer,
                    Flags = 0,
                });
            }
        }

        // ---- BVH, mirroring runtime/src/phys/phys_bvh.cpp ------------------

        private static int BuildBvh(Tri[] triangles, List<Vector3> vertices,
                                    Node[] nodes)
        {
            if (triangles.Length == 0)
            {
                nodes[0] = new Node
                {
                    Min = new Vector3(3.4e38f, 3.4e38f, 3.4e38f),
                    Max = new Vector3(-3.4e38f, -3.4e38f, -3.4e38f),
                    First = 0,
                    Count = 0,
                };
                return 1;
            }
            int count = 1;
            Build(triangles, vertices, nodes, 0, 0, triangles.Length, 0, ref count);
            return count;
        }

        private static void Build(Tri[] tris, List<Vector3> verts, Node[] nodes,
                                  int node, int first, int n, int depth,
                                  ref int nodeCount)
        {
            Vector3 min = new Vector3(3.4e38f, 3.4e38f, 3.4e38f);
            Vector3 max = new Vector3(-3.4e38f, -3.4e38f, -3.4e38f);
            for (int i = 0; i < n; i++)
            {
                Expand(ref min, ref max, verts[tris[first + i].V0]);
                Expand(ref min, ref max, verts[tris[first + i].V1]);
                Expand(ref min, ref max, verts[tris[first + i].V2]);
            }
            nodes[node].Min = min;
            nodes[node].Max = max;

            if (n <= LeafSize || depth >= MaxDepth)
            {
                nodes[node].First = (uint)first;
                nodes[node].Count = (uint)n;
                return;
            }

            // Split the longest axis of the CENTROID bounds, as the runtime
            // does: a few large triangles otherwise stretch the box and pick
            // an axis nothing is spread along.
            Vector3 cmin = new Vector3(3.4e38f, 3.4e38f, 3.4e38f);
            Vector3 cmax = new Vector3(-3.4e38f, -3.4e38f, -3.4e38f);
            for (int i = 0; i < n; i++)
                Expand(ref cmin, ref cmax, Centroid(tris[first + i], verts));
            Vector3 extent = cmax - cmin;
            int axis = 0;
            if (extent.y > extent.x) axis = 1;
            if (Axis(extent, 2) > Axis(extent, axis)) axis = 2;

            int mid = first;
            if (Axis(extent, axis) > 1e-6f)
            {
                float split = (Axis(cmin, axis) + Axis(cmax, axis)) * 0.5f;
                int i2 = first, j = first + n - 1;
                while (i2 <= j)
                {
                    if (Axis(Centroid(tris[i2], verts), axis) < split)
                    {
                        i2++;
                    }
                    else
                    {
                        Tri tmp = tris[i2];
                        tris[i2] = tris[j];
                        tris[j] = tmp;
                        if (j == first) break;
                        j--;
                    }
                }
                mid = i2;
            }
            // Identical centroids never split; halve by count instead. A
            // worse tree, but a finite one -- the same fallback the runtime
            // builder has, and for the same reason.
            if (mid == first || mid == first + n)
                mid = first + n / 2;

            int left = nodeCount++;
            int right = nodeCount++;
            nodes[node].First = (uint)left;
            nodes[node].Count = 0;
            Build(tris, verts, nodes, left, first, mid - first, depth + 1,
                  ref nodeCount);
            Build(tris, verts, nodes, right, mid, first + n - mid, depth + 1,
                  ref nodeCount);
        }

        private static Vector3 Centroid(Tri t, List<Vector3> v) =>
            (v[t.V0] + v[t.V1] + v[t.V2]) / 3f;

        private static float Axis(Vector3 v, int a) =>
            a == 0 ? v.x : (a == 1 ? v.y : v.z);

        private static void Expand(ref Vector3 min, ref Vector3 max, Vector3 p)
        {
            min.x = Mathf.Min(min.x, p.x);
            min.y = Mathf.Min(min.y, p.y);
            min.z = Mathf.Min(min.z, p.z);
            max.x = Mathf.Max(max.x, p.x);
            max.y = Mathf.Max(max.y, p.y);
            max.z = Mathf.Max(max.z, p.z);
        }

        private static float MaxAbs(Vector3 v) =>
            Mathf.Max(Mathf.Abs(v.x), Mathf.Max(Mathf.Abs(v.y), Mathf.Abs(v.z)));

        private static string Path(Component c)
        {
            string path = c.name;
            Transform t = c.transform.parent;
            while (t != null)
            {
                path = t.name + "/" + path;
                t = t.parent;
            }
            return path;
        }

        // ---- serialisation, per docs/formats/p2b-container.md --------------

        private static byte[] Serialize(List<ColliderRecord> colliders,
                                        Node[] nodes, int nodeCount,
                                        Tri[] triangles, List<Vector3> vertices)
        {
            const uint header = 32;
            uint collidersAt = header;
            uint nodesAt = collidersAt + (uint)colliders.Count * 48u;
            uint trianglesAt = nodesAt + (uint)nodeCount * 32u;
            uint verticesAt = trianglesAt + (uint)triangles.Length * 8u;

            using (var stream = new MemoryStream())
            using (var w = new BinaryWriter(stream))
            {
                w.Write((uint)colliders.Count);
                w.Write((uint)nodeCount);
                w.Write((uint)triangles.Length);
                w.Write((uint)vertices.Count);
                w.Write(collidersAt);
                w.Write(nodesAt);
                w.Write(trianglesAt);
                w.Write(verticesAt);

                foreach (ColliderRecord c in colliders)
                {
                    w.Write(c.Kind);
                    uint flags = 0;
                    if (c.IsTrigger) flags |= 1u;
                    if (c.Enabled) flags |= 2u;
                    flags |= (c.Axis & 3u) << 2;
                    w.Write(flags);
                    w.Write(c.Layer);
                    w.Write(c.Entity);
                    WriteVec3(w, c.Center);
                    WriteVec3(w, c.HalfExtents);
                    w.Write(c.Height);
                    w.Write(0u); // reserved
                }
                for (int i = 0; i < nodeCount; i++)
                {
                    WriteVec3(w, nodes[i].Min);
                    w.Write(nodes[i].First);
                    WriteVec3(w, nodes[i].Max);
                    w.Write(nodes[i].Count);
                }
                foreach (Tri t in triangles)
                {
                    w.Write(t.V0);
                    w.Write(t.V1);
                    w.Write(t.V2);
                    w.Write(t.Layer);
                    w.Write(t.Flags);
                }
                foreach (Vector3 v in vertices)
                    WriteVec3(w, v);

                w.Flush();
                return stream.ToArray();
            }
        }

        private static void WriteVec3(BinaryWriter w, Vector3 v)
        {
            w.Write(v.x);
            w.Write(v.y);
            w.Write(v.z);
        }
    }
}
