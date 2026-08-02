// stream: a prioritised, incremental file-read queue with progress
// reporting, plus the first-access recorder that feeds the disc-layout
// planner (plan section 9, M10 task 4).
//
// Why a queue rather than blocking reads: on a console the media is slow
// and seeking is the expensive part, so loading is a background activity
// that shares the frame with a spinning loading screen and, crucially, with
// the music feeder. update() therefore does a BOUNDED amount of work per
// call and returns; nothing here ever blocks for a whole file.
//
// Priority exists for the same reason: the streamed music track and the
// next room's geometry are both in flight, and the music must not wait
// behind a 4 MB mesh.
#pragma once

#include <cstdint>

namespace ps2ur {
namespace stream {

inline constexpr uint32_t kMaxRequests = 32;
inline constexpr uint32_t kMaxPathLength = 64;
// Records enough of a first-access trace to order a disc layout.
inline constexpr uint32_t kMaxTraceEntries = 64;

enum class Priority : uint8_t {
    Low = 0,     // prefetch: whatever is left over
    Normal = 1,  // the level being loaded
    High = 2,    // audio refills and anything the frame is waiting on
};

enum class RequestState : uint8_t {
    // Zero means "this slot has never been used". It matters that the
    // DEFAULT is idle rather than pending: a freshly initialised queue whose
    // slots all read as pending has no free slots at all, and every request
    // is refused (found immediately by the tests).
    Idle = 0,
    Pending,
    Reading,
    Done,
    Failed,
    Cancelled,
};

// Called when a request finishes, on the thread that called update().
typedef void (*CompleteFn)(void* user, uint32_t request, bool ok,
                           uint32_t bytes);

bool init();
void shutdown();
bool initialized();

// Queues a read of the whole file into 'dest'. Returns a request id (never
// 0), or 0 when the queue is full. The buffer must outlive the request.
uint32_t request(const char* path, void* dest, uint32_t capacity,
                 Priority priority, CompleteFn on_complete, void* user);

// Does up to 'byte_budget' bytes of reading, highest priority first, then
// in submission order. Returns the number of requests still outstanding.
// A budget of 0 means "one chunk", which is what a frame loop wants.
uint32_t update(uint32_t byte_budget);

// Runs the queue to completion. For a loading screen that has nothing else
// to do -- it still pumps in bounded steps so a progress callback can draw.
void drain(void (*on_progress)(void* user, float fraction), void* user);

uint32_t pending();
RequestState state(uint32_t request);
uint32_t bytes_transferred(uint32_t request);
void cancel(uint32_t request);
void cancel_all();

// Progress across every request submitted since the queue last went empty,
// 0..1. This is what a loading bar reads.
float progress();

// ---- disc layout planning (M10 task 4) ------------------------------------
//
// The planner needs to know the order files are FIRST touched during a real
// run; mkps2iso can then place them in that order so the drive reads
// forward instead of seeking. The recorder is always on -- it costs one
// comparison per open -- and the trace is dumped by the profiling build.

uint32_t trace_count();
const char* trace_entry(uint32_t index);
void trace_reset();
// Writes the trace as one path per line, the form the layout tool consumes.
// Returns bytes written.
uint32_t trace_dump(char* buffer, uint32_t capacity);

} // namespace stream
} // namespace ps2ur
