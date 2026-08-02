#include "ps2ur/memcard.h"

#include "ps2ur/log.h"
#include "ps2ur/platform.h"

#if defined(PS2UR_PLATFORM_PS2)
#include <libmc.h>

// mcOpen takes the FIO flag values, NOT newlib's fcntl.h ones. They differ
// where it matters most: fio's read-only is 1 while newlib's is 0, so
// including <fcntl.h> here silently opens files in mode 0 -- which is not a
// mode at all, and the card answers with a permission error that looks like
// a write-protected card. Values from ps2sdk common/include/io_common.h.
#define PS2_FIO_O_RDONLY 0x0001
#define PS2_FIO_O_WRONLY 0x0002
#define PS2_FIO_O_CREAT  0x0200
#define PS2_FIO_O_TRUNC  0x0400

extern "C" {
extern unsigned char mcman_irx_start[];
extern unsigned char mcman_irx_end[];
extern unsigned char mcserv_irx_start[];
extern unsigned char mcserv_irx_end[];
}
#endif

namespace ps2ur {
namespace memcard {

namespace {

bool g_initialized = false;

// ---- tiny string helpers (no libc string.h in runtime code) ---------------

uint32_t str_len(const char* s)
{
    uint32_t n = 0;
    while (s != nullptr && s[n] != '\0') {
        ++n;
    }
    return n;
}

bool str_equal(const char* a, const char* b)
{
    if (a == nullptr || b == nullptr) {
        return false;
    }
    uint32_t i = 0;
    for (; a[i] != '\0' && b[i] != '\0'; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return a[i] == b[i];
}

void str_copy(char* dest, const char* src, uint32_t capacity)
{
    uint32_t i = 0;
    for (; src != nullptr && src[i] != '\0' && i + 1u < capacity; ++i) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

// Builds "/directory/file" (file may be null for the directory itself).
//
// libmc paths are RELATIVE TO THE CARD: the port and slot arguments already
// select which card, so a "mc0:" prefix makes the driver look for a
// directory literally named "mc0:" and every write lands nowhere.
void build_path(char* out, uint32_t capacity, uint32_t port,
                const char* directory, const char* file)
{
    (void)port;
    uint32_t at = 0;
    if (at + 1u < capacity) {
        out[at++] = '/';
    }
    for (uint32_t i = 0; directory != nullptr && directory[i] != '\0' &&
                         at + 1u < capacity;
         ++i) {
        out[at++] = directory[i];
    }
    if (file != nullptr) {
        if (at + 1u < capacity) {
            out[at++] = '/';
        }
        for (uint32_t i = 0; file[i] != '\0' && at + 1u < capacity; ++i) {
            out[at++] = file[i];
        }
    }
    out[at] = '\0';
}

#if defined(PS2UR_PLATFORM_PS2)
// Every libmc call is issued then waited on. Returns the driver's result.
int sync_result()
{
    int result = -1;
    mcSync(MC_WAIT, nullptr, &result);
    return result;
}

Status classify(int result)
{
    // The real sceMcRes* codes from libmc-common.h -- worth reading rather
    // than guessing, since -3 is a FULL device and -5 is a permission
    // failure, not the other way round.
    switch (result) {
        case sceMcResSucceed:
            return Status::Ok;
        case sceMcResNoFormat:
            return Status::NotFormatted;
        case sceMcResFullDevice:
            return Status::Full;
        case sceMcResNoEntry:
            return Status::NotFound;
        case sceMcResDeniedPermit:
            return Status::WriteProtected;
        default:
            return result >= 0 ? Status::Ok : Status::Error;
    }
}
#endif

// ---- prefs storage --------------------------------------------------------

enum class PrefType : uint8_t { Int = 0, Float = 1, String = 2 };

struct Pref {
    char key[kMaxKeyLength] = {};
    PrefType type = PrefType::Int;
    int32_t as_int = 0;
    float as_float = 0.0f;
    char as_string[kMaxStringLength] = {};
    bool used = false;
};

Pref g_prefs[kMaxPrefs];

Pref* find_pref(const char* key)
{
    for (uint32_t i = 0; i < kMaxPrefs; ++i) {
        if (g_prefs[i].used && str_equal(g_prefs[i].key, key)) {
            return &g_prefs[i];
        }
    }
    return nullptr;
}

Pref* find_or_add(const char* key)
{
    Pref* existing = find_pref(key);
    if (existing != nullptr) {
        return existing;
    }
    if (key == nullptr || str_len(key) == 0 || str_len(key) >= kMaxKeyLength) {
        return nullptr;
    }
    for (uint32_t i = 0; i < kMaxPrefs; ++i) {
        if (!g_prefs[i].used) {
            g_prefs[i] = Pref{};
            str_copy(g_prefs[i].key, key, kMaxKeyLength);
            g_prefs[i].used = true;
            return &g_prefs[i];
        }
    }
    return nullptr;
}

// Blob layout: 'P','2','P','F', u32 version, u32 count, then per entry:
// u8 type, u8 key_len, u16 value_len, key bytes, value bytes.
constexpr uint32_t kPrefsVersion = 1;

void put_u32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

uint32_t get_u32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

const char* status_text(Status status)
{
    switch (status) {
        case Status::Ok:
            return "ok";
        case Status::NoCard:
            return "no memory card in the slot";
        case Status::NotFormatted:
            return "card is not formatted";
        case Status::Full:
            return "card is full";
        case Status::WriteProtected:
            return "card is write protected";
        case Status::NotFound:
            return "save not found";
        default:
            return "memory card error";
    }
}

bool init()
{
    if (g_initialized) {
        return true;
    }
#if defined(PS2UR_PLATFORM_PS2)
    const int mcman = platform::load_irx(
        "mcman.irx", mcman_irx_start,
        static_cast<unsigned>(mcman_irx_end - mcman_irx_start));
    const int mcserv = platform::load_irx(
        "mcserv.irx", mcserv_irx_start,
        static_cast<unsigned>(mcserv_irx_end - mcserv_irx_start));
    if (mcman < 0 || mcserv < 0) {
        log(LogLevel::Error, "memcard: IOP modules failed (mcman=%d mcserv=%d)",
            mcman, mcserv);
        return false;
    }
    if (mcInit(MC_TYPE_XMC) < 0) {
        log(LogLevel::Error, "memcard: mcInit failed");
        return false;
    }
#endif
    prefs::clear();
    g_initialized = true;
    log(LogLevel::Debug, "memcard: init");
    return true;
}

void shutdown() { g_initialized = false; }
bool initialized() { return g_initialized; }

Status probe(uint32_t port, CardInfo* out_info)
{
    CardInfo info;
    if (port >= kMaxPorts) {
        if (out_info != nullptr) {
            *out_info = info;
        }
        return Status::Error;
    }
#if defined(PS2UR_PLATFORM_PS2)
    int type = 0;
    int free_clusters = 0;
    int format = 0;
    mcGetInfo(static_cast<int>(port), 0, &type, &free_clusters, &format);
    const int result = sync_result();
    // mcGetInfo reports 0 or -1 with the card present; the interesting
    // fields are 'type' and 'format'.
    info.type = type;
    info.present = type == MC_TYPE_PS2 || type == MC_TYPE_PSX;
    info.formatted = format == 1;
    info.free_clusters = free_clusters;
    if (out_info != nullptr) {
        *out_info = info;
    }
    if (!info.present) {
        return Status::NoCard;
    }
    if (!info.formatted) {
        return Status::NotFormatted;
    }
    (void)result;
    return Status::Ok;
#else
    if (out_info != nullptr) {
        *out_info = info;
    }
    return Status::NoCard;
#endif
}

Status format(uint32_t port)
{
#if defined(PS2UR_PLATFORM_PS2)
    if (port >= kMaxPorts) {
        return Status::Error;
    }
    mcFormat(static_cast<int>(port), 0);
    const int result = sync_result();
    if (result < 0) {
        return classify(result);
    }
    log(LogLevel::Warn, "memcard: formatted slot %u -- all saves erased",
        static_cast<unsigned>(port));
    return Status::Ok;
#else
    (void)port;
    return Status::NoCard;
#endif
}

Status create_save(uint32_t port, const char* directory, const char* title)
{
#if defined(PS2UR_PLATFORM_PS2)
    CardInfo info;
    const Status ready = probe(port, &info);
    if (ready != Status::Ok) {
        return ready;
    }
    char path[64];
    build_path(path, sizeof(path), port, directory, nullptr);
    mcMkDir(static_cast<int>(port), 0, path);
    const int made = sync_result();
    // An existing directory is success, not an error: saving twice is normal.
    if (made < 0 && made != -4) {
        // -4 here is "already exists" in mcMkDir's vocabulary; anything else
        // is worth reporting.
        const Status status = classify(made);
        if (status != Status::Ok) {
            log(LogLevel::Warn, "memcard: mkdir '%s' -> %d", path, made);
        }
    }

    // icon.sys is what makes the save visible and named in the browser. A
    // minimal one still needs the title and the icon file names; the icon
    // meshes themselves are written by the exporter (M10 task 3).
    uint8_t icon_sys[964] = {};
    const char header[] = "PS2D";
    for (uint32_t i = 0; i < 4u; ++i) {
        icon_sys[i] = static_cast<uint8_t>(header[i]);
    }
    // Title, in Shift-JIS; ASCII is a subset, which is all we promise.
    const uint32_t title_at = 0xC0;
    for (uint32_t i = 0; i < kMaxTitleLength && title != nullptr &&
                         title[i] != '\0';
         ++i) {
        icon_sys[title_at + i] = static_cast<uint8_t>(title[i]);
    }
    const char icon_name[] = "icon.icn";
    for (uint32_t i = 0; i < 8u; ++i) {
        icon_sys[0x100 + i] = static_cast<uint8_t>(icon_name[i]); // normal
        icon_sys[0x140 + i] = static_cast<uint8_t>(icon_name[i]); // copy
        icon_sys[0x180 + i] = static_cast<uint8_t>(icon_name[i]); // delete
    }
    return write_file(port, directory, "icon.sys", icon_sys, sizeof(icon_sys));
#else
    (void)port;
    (void)directory;
    (void)title;
    return Status::NoCard;
#endif
}

Status write_file(uint32_t port, const char* directory, const char* file,
                  const void* data, uint32_t size)
{
#if defined(PS2UR_PLATFORM_PS2)
    char path[80];
    build_path(path, sizeof(path), port, directory, file);
    mcOpen(static_cast<int>(port), 0, path,
            PS2_FIO_O_WRONLY | PS2_FIO_O_CREAT | PS2_FIO_O_TRUNC);
    const int fd = sync_result();
    if (fd < 0) {
        return classify(fd);
    }
    mcWrite(fd, data, static_cast<int>(size));
    const int written = sync_result();
    mcClose(fd);
    sync_result();
    if (written < 0) {
        return classify(written);
    }
    if (static_cast<uint32_t>(written) != size) {
        return Status::Full; // a short write on a card means out of space
    }
    return Status::Ok;
#else
    (void)port;
    (void)directory;
    (void)file;
    (void)data;
    (void)size;
    return Status::NoCard;
#endif
}

Status read_file(uint32_t port, const char* directory, const char* file,
                 void* data, uint32_t capacity, uint32_t* out_size)
{
    if (out_size != nullptr) {
        *out_size = 0;
    }
#if defined(PS2UR_PLATFORM_PS2)
    char path[80];
    build_path(path, sizeof(path), port, directory, file);
    mcOpen(static_cast<int>(port), 0, path, PS2_FIO_O_RDONLY);
    const int fd = sync_result();
    if (fd < 0) {
        return Status::NotFound;
    }
    // A memory card will NOT short-read: asking for more bytes than the file
    // holds fails the whole call. Size it first, then read exactly that.
    mcSeek(fd, 0, 2 /*SEEK_END*/);
    const int size = sync_result();
    mcSeek(fd, 0, 0 /*SEEK_SET*/);
    sync_result();
    if (size < 0) {
        mcClose(fd);
        sync_result();
        return classify(size);
    }
    const uint32_t want = static_cast<uint32_t>(size) < capacity
                              ? static_cast<uint32_t>(size)
                              : capacity;
    mcRead(fd, data, static_cast<int>(want));
    const int read = sync_result();
    mcClose(fd);
    sync_result();
    if (read < 0) {
        return classify(read);
    }
    if (out_size != nullptr) {
        *out_size = static_cast<uint32_t>(read);
    }
    return Status::Ok;
#else
    (void)port;
    (void)directory;
    (void)file;
    (void)data;
    (void)capacity;
    return Status::NoCard;
#endif
}

bool save_exists(uint32_t port, const char* directory, const char* file)
{
    uint8_t probe_byte = 0;
    uint32_t got = 0;
    return read_file(port, directory, file, &probe_byte, 1, &got) == Status::Ok;
}

Status delete_save(uint32_t port, const char* directory)
{
#if defined(PS2UR_PLATFORM_PS2)
    // The directory has to be emptied before it will go.
    static const char* const kFiles[] = {"icon.sys", "icon.icn", "prefs.dat",
                                         "save.dat"};
    char path[80];
    for (uint32_t i = 0; i < 4u; ++i) {
        build_path(path, sizeof(path), port, directory, kFiles[i]);
        mcDelete(static_cast<int>(port), 0, path);
        sync_result();
    }
    build_path(path, sizeof(path), port, directory, nullptr);
    mcDelete(static_cast<int>(port), 0, path);
    const int result = sync_result();
    return result < 0 ? Status::NotFound : Status::Ok;
#else
    (void)port;
    (void)directory;
    return Status::NoCard;
#endif
}

// ---- prefs ----------------------------------------------------------------

namespace prefs {

void clear()
{
    for (uint32_t i = 0; i < kMaxPrefs; ++i) {
        g_prefs[i] = Pref{};
    }
}

void set_int(const char* key, int32_t value)
{
    Pref* pref = find_or_add(key);
    if (pref != nullptr) {
        pref->type = PrefType::Int;
        pref->as_int = value;
    }
}

void set_float(const char* key, float value)
{
    Pref* pref = find_or_add(key);
    if (pref != nullptr) {
        pref->type = PrefType::Float;
        pref->as_float = value;
    }
}

void set_string(const char* key, const char* value)
{
    Pref* pref = find_or_add(key);
    if (pref != nullptr) {
        pref->type = PrefType::String;
        str_copy(pref->as_string, value, kMaxStringLength);
    }
}

int32_t get_int(const char* key, int32_t fallback)
{
    const Pref* pref = find_pref(key);
    return pref != nullptr && pref->type == PrefType::Int ? pref->as_int
                                                          : fallback;
}

float get_float(const char* key, float fallback)
{
    const Pref* pref = find_pref(key);
    return pref != nullptr && pref->type == PrefType::Float ? pref->as_float
                                                            : fallback;
}

const char* get_string(const char* key, const char* fallback)
{
    const Pref* pref = find_pref(key);
    return pref != nullptr && pref->type == PrefType::String ? pref->as_string
                                                             : fallback;
}

bool has_key(const char* key) { return find_pref(key) != nullptr; }

void delete_key(const char* key)
{
    Pref* pref = find_pref(key);
    if (pref != nullptr) {
        *pref = Pref{};
    }
}

uint32_t count()
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < kMaxPrefs; ++i) {
        if (g_prefs[i].used) {
            ++n;
        }
    }
    return n;
}

uint32_t serialize(void* buffer, uint32_t capacity)
{
    uint8_t* out = static_cast<uint8_t*>(buffer);
    if (out == nullptr || capacity < 12u) {
        return 0;
    }
    out[0] = 'P';
    out[1] = '2';
    out[2] = 'P';
    out[3] = 'F';
    put_u32(out + 4, kPrefsVersion);
    put_u32(out + 8, count());
    uint32_t at = 12;

    for (uint32_t i = 0; i < kMaxPrefs; ++i) {
        const Pref& pref = g_prefs[i];
        if (!pref.used) {
            continue;
        }
        const uint32_t key_len = str_len(pref.key);
        const uint32_t value_len = pref.type == PrefType::String
                                       ? str_len(pref.as_string)
                                       : 4u;
        if (at + 4u + key_len + value_len > capacity) {
            return 0; // refuse to write a truncated store
        }
        out[at++] = static_cast<uint8_t>(pref.type);
        out[at++] = static_cast<uint8_t>(key_len);
        out[at++] = static_cast<uint8_t>(value_len & 0xFF);
        out[at++] = static_cast<uint8_t>((value_len >> 8) & 0xFF);
        for (uint32_t k = 0; k < key_len; ++k) {
            out[at++] = static_cast<uint8_t>(pref.key[k]);
        }
        if (pref.type == PrefType::String) {
            for (uint32_t v = 0; v < value_len; ++v) {
                out[at++] = static_cast<uint8_t>(pref.as_string[v]);
            }
        } else if (pref.type == PrefType::Int) {
            put_u32(out + at, static_cast<uint32_t>(pref.as_int));
            at += 4;
        } else {
            union {
                float f;
                uint32_t u;
            } bits{pref.as_float};
            put_u32(out + at, bits.u);
            at += 4;
        }
    }
    return at;
}

bool deserialize(const void* buffer, uint32_t size)
{
    const uint8_t* in = static_cast<const uint8_t*>(buffer);
    if (in == nullptr || size < 12u || in[0] != 'P' || in[1] != '2' ||
        in[2] != 'P' || in[3] != 'F') {
        return false;
    }
    if (get_u32(in + 4) != kPrefsVersion) {
        return false;
    }
    const uint32_t entries = get_u32(in + 8);
    if (entries > kMaxPrefs) {
        return false;
    }

    // Parse into a scratch set first: a blob that goes bad halfway must not
    // leave the live store half-updated.
    Pref parsed[kMaxPrefs];
    uint32_t at = 12;
    for (uint32_t i = 0; i < entries; ++i) {
        if (at + 4u > size) {
            return false;
        }
        const uint8_t type = in[at];
        const uint32_t key_len = in[at + 1];
        const uint32_t value_len =
            static_cast<uint32_t>(in[at + 2]) |
            (static_cast<uint32_t>(in[at + 3]) << 8);
        at += 4;
        if (type > 2u || key_len == 0 || key_len >= kMaxKeyLength ||
            at + key_len + value_len > size) {
            return false;
        }
        Pref& pref = parsed[i];
        pref = Pref{};
        for (uint32_t k = 0; k < key_len; ++k) {
            pref.key[k] = static_cast<char>(in[at + k]);
        }
        pref.key[key_len] = '\0';
        at += key_len;
        pref.type = static_cast<PrefType>(type);
        if (pref.type == PrefType::String) {
            if (value_len >= kMaxStringLength) {
                return false;
            }
            for (uint32_t v = 0; v < value_len; ++v) {
                pref.as_string[v] = static_cast<char>(in[at + v]);
            }
            pref.as_string[value_len] = '\0';
        } else {
            if (value_len != 4u) {
                return false;
            }
            const uint32_t raw = get_u32(in + at);
            if (pref.type == PrefType::Int) {
                pref.as_int = static_cast<int32_t>(raw);
            } else {
                union {
                    uint32_t u;
                    float f;
                } bits{raw};
                pref.as_float = bits.f;
            }
        }
        at += value_len;
        pref.used = true;
    }

    clear();
    for (uint32_t i = 0; i < entries; ++i) {
        g_prefs[i] = parsed[i];
    }
    return true;
}

Status save(uint32_t port, const char* directory, const char* title)
{
    static uint8_t blob[4096];
    const uint32_t size = serialize(blob, sizeof(blob));
    if (size == 0) {
        return Status::Full;
    }
    const Status created = create_save(port, directory, title);
    if (created != Status::Ok) {
        return created;
    }
    return write_file(port, directory, "prefs.dat", blob, size);
}

Status load(uint32_t port, const char* directory)
{
    static uint8_t blob[4096];
    uint32_t size = 0;
    const Status status =
        read_file(port, directory, "prefs.dat", blob, sizeof(blob), &size);
    if (status != Status::Ok) {
        return status;
    }
    return deserialize(blob, size) ? Status::Ok : Status::Error;
}

} // namespace prefs

} // namespace memcard
} // namespace ps2ur
