#include "ps2ur/p2b.h"
#include "ps2ur/platform.h"

#include "ps2ur/alloc.h"
#include "ps2ur/log.h"

#include <cstdio>
#include <cstring>

namespace ps2ur {
namespace io {

namespace {

// CRC32 (reflected, polynomial 0xEDB88320) one nibble at a time.
//
// This is on the scene-load critical path: parse() checksums every section,
// so a bit-at-a-time loop costs eight shift-and-mask steps per byte. That
// measured 149 ms to parse a 430 KB scene on the EE -- long enough to drain
// the music ring and produce an audible dropout during an async load
// (samples/20-scene-stream; verify-log M10). A nibble table is ~4x faster
// and, unlike the 1 KB byte-wise table, costs 64 bytes of a very small data
// cache that a bulk scan is already thrashing.
//
// constexpr so it lands in .rodata with no initialisation order to get
// wrong. The values are identical to the bitwise loop's by construction.
constexpr uint32_t crc_nibble(uint32_t n)
{
    uint32_t c = n;
    for (int bit = 0; bit < 4; ++bit) {
        c = (c & 1u) != 0u ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
    }
    return c;
}

constexpr uint32_t kCrcTable[16] = {
    crc_nibble(0),  crc_nibble(1),  crc_nibble(2),  crc_nibble(3),
    crc_nibble(4),  crc_nibble(5),  crc_nibble(6),  crc_nibble(7),
    crc_nibble(8),  crc_nibble(9),  crc_nibble(10), crc_nibble(11),
    crc_nibble(12), crc_nibble(13), crc_nibble(14), crc_nibble(15),
};

} // namespace

uint32_t crc32(const void* data, uint32_t size)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < size; ++i) {
        crc ^= p[i];
        crc = (crc >> 4) ^ kCrcTable[crc & 0x0Fu];
        crc = (crc >> 4) ^ kCrcTable[crc & 0x0Fu];
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

#if defined(PS2UR_PLATFORM_PS2)

// Legacy fio, declared here rather than via <fileio.h>: that header refuses
// to compile under newlib ("will lead to problems") because MIXING raw fio
// file descriptors with newlib's is unsafe. A whole-file load that opens,
// reads and closes its own fd never mixes anything.
//
// Why legacy fio at all: PCSX2's 'host:' HLE intercepts the LEGACY fio path.
// ps2sdk's newlib routes open() exclusively through fileXio -- even with
// iomanX+fileXio loaded (platform init does load them), host: stays invisible
// to that route under the emulator, failing instantly with no error. Verified
// empirically 2026-08-01; revisit on real hardware with ps2link, whose host:
// device is a real ioman device that both routes can see.
extern "C" int fioOpen(const char* name, int mode);
extern "C" int fioClose(int fd);
extern "C" int fioRead(int fd, void* buffer, int size);
extern "C" int fioLseek(int fd, int offset, int whence);

const void* load_file(const char* path, Arena& arena, uint32_t* out_size)
{
    if (out_size != nullptr) {
        *out_size = 0;
    }
    const int fd = fioOpen(path, 1 /*FIO_O_RDONLY*/);
    if (fd < 0) {
        log(LogLevel::Error, "io: cannot open '%s' (fio %d)", path, fd);
        return nullptr;
    }
    const int size_i = fioLseek(fd, 0, 2 /*SEEK_END*/);
    fioLseek(fd, 0, 0 /*SEEK_SET*/);
    if (size_i <= 0) {
        fioClose(fd);
        log(LogLevel::Error, "io: '%s' is empty or unsizable", path);
        return nullptr;
    }
    const uint32_t size = static_cast<uint32_t>(size_i);
    void* buffer = arena.alloc(size, 16);
    if (buffer == nullptr) {
        fioClose(fd);
        log(LogLevel::Error, "io: no arena space for %u bytes of '%s'",
            static_cast<unsigned>(size), path);
        return nullptr;
    }
    // fio transfers can be partial; loop like read(2).
    uint32_t done = 0;
    while (done < size) {
        const int got = fioRead(fd, static_cast<uint8_t*>(buffer) + done,
                                static_cast<int>(size - done));
        if (got <= 0) {
            break;
        }
        done += static_cast<uint32_t>(got);
    }
    fioClose(fd);
    if (done != size) {
        log(LogLevel::Error, "io: short read on '%s' (%u of %u)", path,
            static_cast<unsigned>(done), static_cast<unsigned>(size));
        return nullptr;
    }
    if (out_size != nullptr) {
        *out_size = size;
    }
    return buffer;
}

#else // host build

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

#endif // PS2UR_PLATFORM_PS2

// ---- media path resolution -------------------------------------------------

namespace {

// A device prefix is "word:" before any path separator: "host:", "cdrom0:",
// "mass:", "mc0:". A bare name has none.
bool has_device_prefix(const char* name)
{
    for (const char* p = name; *p != '\0'; ++p) {
        if (*p == ':') {
            return true;
        }
        if (*p == '/' || *p == '\\') {
            return false;
        }
    }
    return false;
}

bool append(char* out, uint32_t capacity, uint32_t& len, const char* text,
            bool upper)
{
    for (const char* p = text; *p != '\0'; ++p) {
        if (len + 1 >= capacity) {
            return false;
        }
        char c = *p;
        if (upper && c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        out[len++] = c;
    }
    out[len] = '\0';
    return true;
}

bool can_open(const char* path)
{
#if defined(PS2UR_PLATFORM_PS2)
    const int fd = fioOpen(path, 1 /*FIO_O_RDONLY*/);
    if (fd < 0) {
        return false;
    }
    fioClose(fd);
    return true;
#else
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }
    fclose(f);
    return true;
#endif
}

} // namespace

bool resolve_media_path(const char* name, char* out, uint32_t capacity)
{
    if (name == nullptr || out == nullptr || capacity == 0) {
        return false;
    }
    out[0] = '\0';
    if (has_device_prefix(name)) {
        uint32_t len = 0;
        return append(out, capacity, len, name, false) && can_open(out);
    }

    // Ordered cheapest-and-likeliest first. cdrom0 is last because a disc
    // seek costs real time and a dev build almost never wants it.
    struct Candidate {
        const char* prefix;
        const char* suffix;
        bool upper;
        bool host;
    };
    static const Candidate kCandidates[] = {
        {"host:", "", false, true},
        {"", "", false, false},
        {"cdrom0:\\", ";1", true, false},
    };

    for (const Candidate& c : kCandidates) {
        // A disc-only build never asks host: (platform policy, set at boot
        // from the build profile), so where a file resolves does not depend
        // on what the emulator happens to be serving.
        if (c.host && !platform::host_media_enabled()) {
            continue;
        }
        uint32_t len = 0;
        // The prefix and the ;1 suffix keep their own case; only the file
        // name is upper-cased for ISO 9660.
        if (!append(out, capacity, len, c.prefix, false) ||
            !append(out, capacity, len, name, c.upper) ||
            !append(out, capacity, len, c.suffix, false)) {
            continue;
        }
        if (can_open(out)) {
            return true;
        }
    }
    out[0] = '\0';
    return false;
}

} // namespace io
} // namespace ps2ur
