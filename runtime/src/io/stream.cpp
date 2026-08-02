#include "ps2ur/stream.h"

#include "ps2ur/log.h"

#if defined(PS2UR_PLATFORM_PS2)
// Legacy fio: newlib routes through fileXio, which PCSX2's host: HLE does
// not serve (verify-log, M5). Declared here rather than including the
// ps2sdk header, whose guard forbids direct fio use.
extern "C" {
int fioOpen(const char* name, int mode);
int fioClose(int fd);
int fioRead(int fd, void* buf, int size);
int fioLseek(int fd, int offset, int whence);
}
#else
#include <cstdio>
#endif

namespace ps2ur {
namespace stream {

namespace {

// One chunk per update() call by default: big enough to make progress,
// small enough that a frame never disappears into the drive.
constexpr uint32_t kDefaultChunk = 32 * 1024;

bool g_initialized = false;

struct Request {
    uint32_t id = 0;
    char path[kMaxPathLength] = {};
    uint8_t* dest = nullptr;
    uint32_t capacity = 0;
    uint32_t transferred = 0;
    uint32_t size = 0; // total, once known
    Priority priority = Priority::Normal;
    RequestState state = RequestState::Idle;
    CompleteFn on_complete = nullptr;
    void* user = nullptr;
    int32_t handle = -1;
    uint32_t sequence = 0; // submission order, for ties
};

Request g_requests[kMaxRequests];
uint32_t g_next_id = 1;
uint32_t g_next_sequence = 0;

// Progress accounting for the current batch.
uint32_t g_batch_total = 0;
uint32_t g_batch_done = 0;

char g_trace[kMaxTraceEntries][kMaxPathLength];
uint32_t g_trace_count = 0;

void str_copy(char* dest, const char* src, uint32_t capacity)
{
    uint32_t i = 0;
    for (; src != nullptr && src[i] != '\0' && i + 1u < capacity; ++i) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

bool str_equal(const char* a, const char* b)
{
    uint32_t i = 0;
    for (; a[i] != '\0' && b[i] != '\0'; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return a[i] == b[i];
}

// Records the first time a path is opened. Repeat opens are ignored: the
// layout planner cares about first touch, which is what determines where a
// file should sit on the disc.
void trace_first_access(const char* path)
{
    for (uint32_t i = 0; i < g_trace_count; ++i) {
        if (str_equal(g_trace[i], path)) {
            return;
        }
    }
    if (g_trace_count < kMaxTraceEntries) {
        str_copy(g_trace[g_trace_count], path, kMaxPathLength);
        ++g_trace_count;
    }
}

// ---- platform file access -------------------------------------------------

int32_t file_open(const char* path)
{
#if defined(PS2UR_PLATFORM_PS2)
    return fioOpen(path, 1 /*O_RDONLY*/);
#else
    FILE* f = std::fopen(path, "rb");
    return f == nullptr ? -1 : static_cast<int32_t>(
                                   reinterpret_cast<intptr_t>(f) & 0x7FFFFFFF);
#endif
}

#if !defined(PS2UR_PLATFORM_PS2)
// The host keeps the real FILE* beside the handle, since a pointer does not
// fit an int32 portably.
FILE* g_host_files[kMaxRequests] = {};
#endif

int32_t file_size(int32_t handle, uint32_t slot)
{
#if defined(PS2UR_PLATFORM_PS2)
    (void)slot;
    const int end = fioLseek(handle, 0, 2 /*SEEK_END*/);
    fioLseek(handle, 0, 0 /*SEEK_SET*/);
    return end;
#else
    (void)handle;
    FILE* f = g_host_files[slot];
    if (f == nullptr) {
        return -1;
    }
    std::fseek(f, 0, SEEK_END);
    const long end = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    return static_cast<int32_t>(end);
#endif
}

int32_t file_read(int32_t handle, uint32_t slot, void* dest, uint32_t bytes)
{
#if defined(PS2UR_PLATFORM_PS2)
    (void)slot;
    return fioRead(handle, dest, static_cast<int>(bytes));
#else
    (void)handle;
    FILE* f = g_host_files[slot];
    if (f == nullptr) {
        return -1;
    }
    return static_cast<int32_t>(std::fread(dest, 1, bytes, f));
#endif
}

void file_close(int32_t handle, uint32_t slot)
{
#if defined(PS2UR_PLATFORM_PS2)
    (void)slot;
    fioClose(handle);
#else
    (void)handle;
    if (g_host_files[slot] != nullptr) {
        std::fclose(g_host_files[slot]);
        g_host_files[slot] = nullptr;
    }
#endif
}

// Picks the next request to service: highest priority, then submission
// order. Starvation is not a concern because high-priority work is short
// (audio refills) and the queue is small.
int32_t pick_request()
{
    int32_t best = -1;
    for (uint32_t i = 0; i < kMaxRequests; ++i) {
        const Request& r = g_requests[i];
        if (r.state != RequestState::Pending && r.state != RequestState::Reading) {
            continue;
        }
        if (best < 0) {
            best = static_cast<int32_t>(i);
            continue;
        }
        const Request& b = g_requests[best];
        if (r.priority > b.priority ||
            (r.priority == b.priority && r.sequence < b.sequence)) {
            best = static_cast<int32_t>(i);
        }
    }
    return best;
}

void finish(Request& r, RequestState state)
{
    if (r.handle >= 0) {
        file_close(r.handle, static_cast<uint32_t>(&r - g_requests));
        r.handle = -1;
    }
    r.state = state;
    ++g_batch_done;
    if (r.on_complete != nullptr) {
        r.on_complete(r.user, r.id, state == RequestState::Done, r.transferred);
    }
    // The batch resets once nothing is outstanding, so a later load starts
    // its progress bar from zero.
    if (pending() == 0) {
        g_batch_total = 0;
        g_batch_done = 0;
    }
}

} // namespace

bool init()
{
    if (g_initialized) {
        return true;
    }
    for (uint32_t i = 0; i < kMaxRequests; ++i) {
        g_requests[i] = Request{};
    }
    g_next_id = 1;
    g_next_sequence = 0;
    g_batch_total = 0;
    g_batch_done = 0;
    g_initialized = true;
    return true;
}

void shutdown()
{
    cancel_all();
    g_initialized = false;
}

bool initialized() { return g_initialized; }

uint32_t request(const char* path, void* dest, uint32_t capacity,
                 Priority priority, CompleteFn on_complete, void* user)
{
    if (!g_initialized || path == nullptr || dest == nullptr || capacity == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < kMaxRequests; ++i) {
        Request& r = g_requests[i];
        if (r.state == RequestState::Pending || r.state == RequestState::Reading) {
            continue;
        }
        r = Request{};
        r.id = g_next_id++;
        if (g_next_id == 0) {
            g_next_id = 1;
        }
        str_copy(r.path, path, kMaxPathLength);
        r.dest = static_cast<uint8_t*>(dest);
        r.capacity = capacity;
        r.priority = priority;
        r.state = RequestState::Pending;
        r.on_complete = on_complete;
        r.user = user;
        r.sequence = g_next_sequence++;
        ++g_batch_total;
        return r.id;
    }
    return 0; // queue full: the caller decides whether that is fatal
}

uint32_t update(uint32_t byte_budget)
{
    if (!g_initialized) {
        return 0;
    }
    uint32_t budget = byte_budget == 0 ? kDefaultChunk : byte_budget;

    while (budget > 0) {
        const int32_t index = pick_request();
        if (index < 0) {
            break;
        }
        Request& r = g_requests[index];
        const uint32_t slot = static_cast<uint32_t>(index);

        if (r.state == RequestState::Pending) {
            trace_first_access(r.path);
#if !defined(PS2UR_PLATFORM_PS2)
            g_host_files[slot] = std::fopen(r.path, "rb");
            r.handle = g_host_files[slot] != nullptr ? 1 : -1;
#else
            r.handle = file_open(r.path);
#endif
            if (r.handle < 0) {
                finish(r, RequestState::Failed);
                continue;
            }
            const int32_t size = file_size(r.handle, slot);
            if (size < 0) {
                finish(r, RequestState::Failed);
                continue;
            }
            r.size = static_cast<uint32_t>(size);
            if (r.size > r.capacity) {
                // Refusing beats overrunning the caller's buffer.
                log(LogLevel::Error, "stream: '%s' is %u bytes, buffer is %u",
                    r.path, static_cast<unsigned>(r.size),
                    static_cast<unsigned>(r.capacity));
                finish(r, RequestState::Failed);
                continue;
            }
            r.state = RequestState::Reading;
        }

        const uint32_t remaining = r.size - r.transferred;
        const uint32_t want = remaining < budget ? remaining : budget;
        if (want == 0) {
            finish(r, RequestState::Done);
            continue;
        }
        const int32_t got = file_read(r.handle, slot, r.dest + r.transferred,
                                      want);
        if (got <= 0) {
            finish(r, RequestState::Failed);
            continue;
        }
        r.transferred += static_cast<uint32_t>(got);
        budget -= static_cast<uint32_t>(got);
        if (r.transferred >= r.size) {
            finish(r, RequestState::Done);
        }
    }
    return pending();
}

void drain(void (*on_progress)(void* user, float fraction), void* user)
{
    uint32_t guard = 0;
    while (pending() > 0 && guard < 100000u) {
        update(kDefaultChunk);
        if (on_progress != nullptr) {
            on_progress(user, progress());
        }
        ++guard;
    }
}

uint32_t pending()
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < kMaxRequests; ++i) {
        if (g_requests[i].state == RequestState::Pending ||
            g_requests[i].state == RequestState::Reading) {
            ++n;
        }
    }
    return n;
}

RequestState state(uint32_t id)
{
    for (uint32_t i = 0; i < kMaxRequests; ++i) {
        if (g_requests[i].id == id && g_requests[i].id != 0) {
            return g_requests[i].state;
        }
    }
    return RequestState::Failed;
}

uint32_t bytes_transferred(uint32_t id)
{
    for (uint32_t i = 0; i < kMaxRequests; ++i) {
        if (g_requests[i].id == id && g_requests[i].id != 0) {
            return g_requests[i].transferred;
        }
    }
    return 0;
}

void cancel(uint32_t id)
{
    for (uint32_t i = 0; i < kMaxRequests; ++i) {
        Request& r = g_requests[i];
        if (r.id == id && (r.state == RequestState::Pending ||
                           r.state == RequestState::Reading)) {
            finish(r, RequestState::Cancelled);
            return;
        }
    }
}

void cancel_all()
{
    for (uint32_t i = 0; i < kMaxRequests; ++i) {
        Request& r = g_requests[i];
        if (r.state == RequestState::Pending || r.state == RequestState::Reading) {
            finish(r, RequestState::Cancelled);
        }
    }
    g_batch_total = 0;
    g_batch_done = 0;
}

float progress()
{
    if (g_batch_total == 0) {
        return 1.0f;
    }
    const float fraction = static_cast<float>(g_batch_done) /
                           static_cast<float>(g_batch_total);
    return fraction > 1.0f ? 1.0f : fraction;
}

uint32_t trace_count() { return g_trace_count; }

const char* trace_entry(uint32_t index)
{
    return index < g_trace_count ? g_trace[index] : "";
}

void trace_reset() { g_trace_count = 0; }

uint32_t trace_dump(char* buffer, uint32_t capacity)
{
    uint32_t at = 0;
    for (uint32_t i = 0; i < g_trace_count; ++i) {
        for (uint32_t k = 0; g_trace[i][k] != '\0' && at + 1u < capacity; ++k) {
            buffer[at++] = g_trace[i][k];
        }
        if (at + 1u < capacity) {
            buffer[at++] = '\n';
        }
    }
    if (capacity > 0) {
        buffer[at < capacity ? at : capacity - 1] = '\0';
    }
    return at;
}

} // namespace stream
} // namespace ps2ur
