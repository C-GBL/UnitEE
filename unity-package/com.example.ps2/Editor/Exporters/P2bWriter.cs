// .p2b container writer (plan section 10; spec: docs/formats/p2b-container.md).
//
// The C++ reader and its tests were written against the same document, so a
// drift on either side fails a test rather than drawing garbage on target.
using System;
using System.Collections.Generic;
using System.IO;

namespace Ps2.Editor
{
    internal sealed class P2bWriter
    {
        public const uint Magic = 0x43423250; // 'P2BC' little-endian
        public const uint SectionMesh = 0x4853454D;     // 'MESH'
        public const uint SectionTex = 0x20584554;      // 'TEX '
        public const uint SectionScene = 0x4E454353;    // 'SCEN'
        public const uint SectionMaterial = 0x4C54414D; // 'MATL'
        public const uint SectionScripts = 0x50524353;  // 'SCRP' (M7)
        public const uint SectionSkeleton = 0x4C454B53;   // 'SKEL' (M9)
        public const uint SectionClip = 0x4D494E41;       // 'ANIM' (M9)
        public const uint SectionController = 0x4C525443; // 'CTRL' (M9)
        public const uint SectionSkinnedMesh = 0x534D4B53; // 'SKMS' (M9)

        private struct Section
        {
            public uint Type;
            public byte[] Payload;
            public ulong NameHash;
        }

        private readonly List<Section> m_sections = new List<Section>();

        public int AddSection(uint type, byte[] payload, string name = null)
        {
            m_sections.Add(new Section
            {
                Type = type,
                Payload = payload,
                NameHash = name == null ? 0UL : Fnv1a64(name),
            });
            return m_sections.Count - 1;
        }

        public void Write(string path)
        {
            int count = m_sections.Count;
            uint tableEnd = 32u + (uint)count * 32u;
            var offsets = new uint[count];
            uint cursor = Align2048(tableEnd);
            for (int i = 0; i < count; i++)
            {
                offsets[i] = cursor;
                cursor = Align2048(cursor + (uint)m_sections[i].Payload.Length);
            }

            var bytes = new byte[cursor];
            using (var ms = new MemoryStream(bytes))
            using (var w = new BinaryWriter(ms))
            {
                w.Write(Magic);
                w.Write((ushort)1);
                w.Write((ushort)0);
                w.Write(cursor);       // total_size
                w.Write((uint)count);
                w.Write(0u);           // flags
                w.Write(new byte[12]); // reserved

                for (int i = 0; i < count; i++)
                {
                    var s = m_sections[i];
                    w.Write(s.Type);
                    w.Write(offsets[i]);
                    w.Write((uint)s.Payload.Length);
                    w.Write((uint)s.Payload.Length); // size_in_ram == on_disc
                    w.Write(Crc32(s.Payload));
                    w.Write(0u); // flags
                    w.Write(s.NameHash);
                }
                for (int i = 0; i < count; i++)
                {
                    ms.Position = offsets[i];
                    w.Write(m_sections[i].Payload);
                }
            }
            File.WriteAllBytes(path, bytes);
        }

        private static uint Align2048(uint v) => (v + 2047u) & ~2047u;

        // Same CRC-32 (reflected, poly 0xEDB88320) as runtime/src/io/p2b.cpp.
        public static uint Crc32(byte[] data)
        {
            uint crc = 0xFFFFFFFFu;
            foreach (byte b in data)
            {
                crc ^= b;
                for (int bit = 0; bit < 8; bit++)
                {
                    uint mask = (uint)-(int)(crc & 1);
                    crc = (crc >> 1) ^ (0xEDB88320u & mask);
                }
            }
            return ~crc;
        }

        public static ulong Fnv1a64(string s)
        {
            ulong h = 14695981039346656037UL;
            foreach (char c in s)
            {
                h ^= (byte)c;
                h *= 1099511628211UL;
            }
            return h;
        }
    }

    // Little-endian struct emitter shared by the exporters.
    internal sealed class ByteBuffer
    {
        private readonly MemoryStream m_stream = new MemoryStream();
        private readonly BinaryWriter m_writer;

        public ByteBuffer() { m_writer = new BinaryWriter(m_stream); }

        public long Position => m_stream.Position;
        public void U8(byte v) => m_writer.Write(v);
        public void U16(ushort v) => m_writer.Write(v);
        public void U32(uint v) => m_writer.Write(v);
        public void I32(int v) => m_writer.Write(v);
        public void F32(float v) => m_writer.Write(v);
        public void U64(ulong v) => m_writer.Write(v);
        public void Bytes(byte[] v) => m_writer.Write(v);

        public void PadTo(int alignment)
        {
            while (m_stream.Position % alignment != 0)
            {
                m_writer.Write((byte)0);
            }
        }

        public byte[] ToArray() => m_stream.ToArray();
    }
}
