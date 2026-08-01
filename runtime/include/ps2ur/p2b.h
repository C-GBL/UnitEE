// .p2b container reader (plan section 10; spec: docs/formats/p2b-container.md).
//
// Zero-copy: parse() takes a buffer that outlives the reader and hands out
// views into it. Every field is treated as hostile -- this is the surface the
// M5 fuzz test hammers, because a malformed file must produce a clean failure,
// never a wild pointer on a machine with no memory protection to save you.
#pragma once

#include <cstdint>

namespace ps2ur {

class Arena;

namespace io {

constexpr uint32_t fourcc(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
}

inline constexpr uint32_t kP2bMagic = fourcc('P', '2', 'B', 'C');
inline constexpr uint16_t kP2bVersionMajor = 1;

inline constexpr uint32_t kSectionMesh = fourcc('M', 'E', 'S', 'H');
inline constexpr uint32_t kSectionTex = fourcc('T', 'E', 'X', ' ');
inline constexpr uint32_t kSectionScene = fourcc('S', 'C', 'E', 'N');
inline constexpr uint32_t kSectionMaterial = fourcc('M', 'A', 'T', 'L');

// Plain CRC-32 (reflected, poly 0xEDB88320), the same the writer uses.
uint32_t crc32(const void* data, uint32_t size);

struct P2bSection {
    uint32_t type = 0;
    const uint8_t* data = nullptr;
    uint32_t size = 0;
    uint64_t name_hash = 0;
};

class P2bFile {
public:
    static constexpr uint32_t kMaxSections = 128;

    // Parses and validates. 'data' must be 16-byte aligned (sections are used
    // in place as qwords) and outlive this object. verify_checksums costs one
    // pass over every payload; skip it only for data already trusted.
    bool parse(const void* data, uint32_t size, bool verify_checksums = true);

    // A human-readable reason for the last parse() failure.
    const char* error() const { return m_error; }

    uint32_t section_count() const { return m_count; }
    const P2bSection& section(uint32_t i) const { return m_sections[i]; }

    // Sections of one type keep their file order; 'index' is within that type.
    uint32_t count_of(uint32_t type) const;
    const P2bSection* find(uint32_t type, uint32_t index = 0) const;

private:
    P2bSection m_sections[kMaxSections];
    uint32_t m_count = 0;
    const char* m_error = "";
};

// Loads a whole file into 'arena' (16-byte aligned). On the PS2 build the
// path goes through newlib/fio, so "host:name.p2b" reads from the directory
// PCSX2 derives from the ELF -- which is exactly where run-emu-test.sh stages
// support files. Returns null on failure and reports the size in *out_size.
const void* load_file(const char* path, Arena& arena, uint32_t* out_size);

} // namespace io
} // namespace ps2ur
