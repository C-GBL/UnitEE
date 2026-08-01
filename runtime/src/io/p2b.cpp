#include "ps2ur/p2b.h"

#include "ps2ur/alloc.h"
#include "ps2ur/log.h"

#include <cstdio>
#include <cstring>

namespace ps2ur {
namespace io {

uint32_t crc32(const void* data, uint32_t size)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < size; ++i) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = static_cast<uint32_t>(-(static_cast<int32_t>(crc) & 1));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

namespace {

uint32_t read_u32(const uint8_t* p)
{
    // Byte-wise: the header is not guaranteed aligned for direct loads, and
    // the EE faults on misaligned word access.
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t read_u64(const uint8_t* p)
{
    return static_cast<uint64_t>(read_u32(p)) |
           (static_cast<uint64_t>(read_u32(p + 4)) << 32);
}

} // namespace

bool P2bFile::parse(const void* data, uint32_t size, bool verify_checksums)
{
    m_count = 0;
    m_error = "";

    if (data == nullptr) {
        m_error = "null buffer";
        return false;
    }
    const uint8_t* base = static_cast<const uint8_t*>(data);
    if (size < 32u) {
        m_error = "smaller than the header";
        return false;
    }
    if (read_u32(base + 0) != kP2bMagic) {
        m_error = "bad magic";
        return false;
    }
    const uint32_t version_major = base[4] | (static_cast<uint32_t>(base[5]) << 8);
    if (version_major != kP2bVersionMajor) {
        m_error = "unsupported version";
        return false;
    }
    const uint32_t total_size = read_u32(base + 8);
    if (total_size != size) {
        m_error = "total_size does not match the buffer";
        return false;
    }
    const uint32_t count = read_u32(base + 12);
    if (count > kMaxSections) {
        m_error = "too many sections";
        return false;
    }
    // Table must fit: 32 + count*32, guarded against overflow.
    if (count > (size - 32u) / 32u) {
        m_error = "section table exceeds the file";
        return false;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = base + 32u + i * 32u;
        P2bSection s;
        s.type = read_u32(e + 0);
        const uint32_t offset = read_u32(e + 4);
        s.size = read_u32(e + 8);
        const uint32_t size_in_ram = read_u32(e + 12);
        const uint32_t checksum = read_u32(e + 16);
        s.name_hash = read_u64(e + 24);

        if (size_in_ram != s.size) {
            m_error = "compressed sections are not supported in v1";
            return false;
        }
        if ((offset & 2047u) != 0) {
            m_error = "section payload not 2048-aligned";
            return false;
        }
        // Bounds: offset + size <= file size, overflow-safe.
        if (offset > size || s.size > size - offset) {
            m_error = "section payload outside the file";
            return false;
        }
        s.data = base + offset;
        if (verify_checksums && crc32(s.data, s.size) != checksum) {
            m_error = "section checksum mismatch";
            return false;
        }
        m_sections[m_count++] = s;
    }
    return true;
}

uint32_t P2bFile::count_of(uint32_t type) const
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < m_count; ++i) {
        if (m_sections[i].type == type) {
            n++;
        }
    }
    return n;
}

const P2bSection* P2bFile::find(uint32_t type, uint32_t index) const
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < m_count; ++i) {
        if (m_sections[i].type == type) {
            if (n == index) {
                return &m_sections[i];
            }
            n++;
        }
    }
    return nullptr;
}

const void* load_file(const char* path, Arena& arena, uint32_t* out_size)
{
    if (out_size != nullptr) {
        *out_size = 0;
    }
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        log(LogLevel::Error, "io: cannot open '%s'", path);
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    const long size_l = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size_l <= 0) {
        fclose(f);
        log(LogLevel::Error, "io: '%s' is empty or unsizable", path);
        return nullptr;
    }
    const uint32_t size = static_cast<uint32_t>(size_l);
    void* buffer = arena.alloc(size, 16);
    if (buffer == nullptr) {
        fclose(f);
        log(LogLevel::Error, "io: no arena space for %u bytes of '%s'",
            static_cast<unsigned>(size), path);
        return nullptr;
    }
    const size_t got = fread(buffer, 1, size, f);
    fclose(f);
    if (got != size) {
        log(LogLevel::Error, "io: short read on '%s' (%u of %u)",
            path, static_cast<unsigned>(got), static_cast<unsigned>(size));
        return nullptr;
    }
    if (out_size != nullptr) {
        *out_size = size;
    }
    return buffer;
}

} // namespace io
} // namespace ps2ur
