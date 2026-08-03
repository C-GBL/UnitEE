// PlayerPrefs half of the managed<->native boundary (M12.5 task 3).
//
// The store itself is M10's memcard::prefs -- a flat key/value table
// serialised into one file on the card, host-testable by design. This file
// only carries it across the boundary.
//
// Keys and string values cross as 'cstr' (input-only, copied by the native
// store on set). ADR-002 reserved cstr for diagnostic paths; prefs widen
// that to persistence keys deliberately: the calls are cold (a handful at
// boot and on save screens), the strings are tiny (the store caps keys at
// 24 bytes and values at 48), and the alternative -- hashing keys -- would
// make GetString impossible.
//
// Strings OUT of native have no bridge type at all (a returned pointer has
// an owner, and the boundary refuses to have one), so GetString reads
// byte-wise: length once, then one call per byte, cached managed-side. Ugly
// on purpose and cheap in practice -- a 48-byte value read once at boot.
#include "ps2ur/bridge.h"

#include "ps2ur/memcard.h"

#include "generated_bridge.h"

#include <cstring>

namespace {

using namespace ps2ur;

// Where Save() writes, named once by the host from game_config.h: the
// on-card directory is the disc serial (the browser convention) and the
// title is what the browser displays.
char g_directory[32] = "PS2GAME";
char g_title[memcard::kMaxTitleLength + 1] = "PS2 Game";

void copy_str(char* dst, const char* src, uint32_t capacity)
{
    if (src == nullptr || capacity == 0) {
        return;
    }
    uint32_t i = 0;
    for (; i + 1 < capacity && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

} // namespace

namespace ps2ur {
namespace bridge {

void prefs_set_identity(const char* directory, const char* title)
{
    copy_str(g_directory, directory, sizeof(g_directory));
    copy_str(g_title, title, sizeof(g_title));
}

} // namespace bridge
} // namespace ps2ur

// ---- C surface (bridge-api.json owns the signatures) -----------------------

extern "C" void ps2ur_prefs_set_int(const char* key, int32_t value)
{
    memcard::prefs::set_int(key, value);
}

extern "C" void ps2ur_prefs_set_float(const char* key, float value)
{
    memcard::prefs::set_float(key, value);
}

extern "C" void ps2ur_prefs_set_string(const char* key, const char* value)
{
    memcard::prefs::set_string(key, value);
}

extern "C" int32_t ps2ur_prefs_get_int(const char* key, int32_t fallback)
{
    return memcard::prefs::get_int(key, fallback);
}

extern "C" float ps2ur_prefs_get_float(const char* key, float fallback)
{
    return memcard::prefs::get_float(key, fallback);
}

extern "C" int32_t ps2ur_prefs_has_key(const char* key)
{
    return memcard::prefs::has_key(key) ? 1 : 0;
}

extern "C" void ps2ur_prefs_delete_key(const char* key)
{
    memcard::prefs::delete_key(key);
}

extern "C" void ps2ur_prefs_delete_all()
{
    memcard::prefs::clear();
}

extern "C" int32_t ps2ur_prefs_string_length(const char* key)
{
    const char* value = memcard::prefs::get_string(key, nullptr);
    return value == nullptr ? -1 : static_cast<int32_t>(std::strlen(value));
}

extern "C" int32_t ps2ur_prefs_string_byte(const char* key, int32_t index)
{
    const char* value = memcard::prefs::get_string(key, nullptr);
    if (value == nullptr || index < 0) {
        return -1;
    }
    for (int32_t i = 0; i < index; ++i) {
        if (value[i] == '\0') {
            return -1;
        }
    }
    return static_cast<int32_t>(
        static_cast<unsigned char>(value[index]));
}

extern "C" int32_t ps2ur_prefs_save()
{
    if (!memcard::initialized()) {
        return static_cast<int32_t>(memcard::Status::Error);
    }
    return static_cast<int32_t>(
        memcard::prefs::save(0, g_directory, g_title));
}
