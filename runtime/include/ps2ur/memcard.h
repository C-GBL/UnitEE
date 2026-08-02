// memcard: PlayStation 2 memory-card saves, plus a PlayerPrefs-equivalent
// on top (plan section 9, M10 task 3).
//
// Every libmc call is asynchronous: you issue it and then mcSync() for the
// result. This module hides that behind blocking calls, because a save is
// something a game does at a deliberate moment, not in the middle of a
// frame -- and because a half-finished async save is exactly how card data
// gets corrupted.
//
// Save layout follows the console convention: one DIRECTORY per title on
// the card, containing an icon.sys plus the save files themselves. The
// browser shows the directory, so a title that writes loose files in the
// root is both rude and unbrowsable.
#pragma once

#include <cstdint>

namespace ps2ur {
namespace memcard {

inline constexpr uint32_t kMaxPorts = 2;
// The PS2 browser truncates longer names, and 32 is what fits its layout.
inline constexpr uint32_t kMaxTitleLength = 32;

enum class Status : int32_t {
    Ok = 0,
    NoCard,        // nothing in the slot
    NotFormatted,  // present but never formatted by the browser
    Full,          // not enough free space or directory entries
    WriteProtected,
    NotFound,      // the save or file does not exist
    Error,         // anything the driver reported that we cannot classify
};

const char* status_text(Status status);

struct CardInfo {
    bool present = false;
    bool formatted = false;
    int32_t free_clusters = 0;
    int32_t type = 0;
};

bool init();
void shutdown();
bool initialized();

// Looks at a slot. Never blocks longer than the driver's own timeout, and
// reports a missing or unformatted card as data rather than as an error --
// both are normal states a game must handle on screen.
Status probe(uint32_t port, CardInfo* out_info);

// Creates the save directory if it is missing and writes an icon.sys so the
// console browser shows the save with a title. 'title' is what the browser
// displays; 'directory' is the on-card name (conventionally the title id).
Status create_save(uint32_t port, const char* directory, const char* title);

// Formats a card. DESTRUCTIVE: it erases every save on it, so a game must
// only ever call this after the player has confirmed on a screen that says
// so. Exposed because "card present but not formatted" is a real state the
// console browser can leave a card in, and the plan requires handling it.
Status format(uint32_t port);

Status write_file(uint32_t port, const char* directory, const char* file,
                  const void* data, uint32_t size);
Status read_file(uint32_t port, const char* directory, const char* file,
                 void* data, uint32_t capacity, uint32_t* out_size);
Status delete_save(uint32_t port, const char* directory);
bool save_exists(uint32_t port, const char* directory, const char* file);

// ---- PlayerPrefs-equivalent ------------------------------------------------
//
// A flat key/value store serialised into one file on the card. The
// serialisation is deliberately independent of libmc so it can be tested on
// the host: format bugs are the kind that quietly eat a player's progress.

inline constexpr uint32_t kMaxPrefs = 64;
inline constexpr uint32_t kMaxKeyLength = 24;
inline constexpr uint32_t kMaxStringLength = 48;

namespace prefs {

void clear();
void set_int(const char* key, int32_t value);
void set_float(const char* key, float value);
void set_string(const char* key, const char* value);
int32_t get_int(const char* key, int32_t fallback);
float get_float(const char* key, float fallback);
const char* get_string(const char* key, const char* fallback);
bool has_key(const char* key);
void delete_key(const char* key);
uint32_t count();

// Serialises into 'buffer'; returns the byte count, or 0 if it will not fit.
uint32_t serialize(void* buffer, uint32_t capacity);
// Parses a blob written by serialize(). Rejects anything malformed rather
// than half-loading it -- a corrupt save must read as "no save".
bool deserialize(const void* buffer, uint32_t size);

// Card-backed convenience wrappers.
Status save(uint32_t port, const char* directory, const char* title);
Status load(uint32_t port, const char* directory);

} // namespace prefs

} // namespace memcard
} // namespace ps2ur
