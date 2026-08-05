// Memory instrumentation (plan section 9, M13 task 2): high-water marks per
// allocator, a fragmentation report, a memory map screen, and the budget
// check that plan section 15.1 exists to be measured against.
//
// The registry does not own or wrap anything. Allocators are registered by
// pointer and polled on demand, so instrumentation costs nothing at
// allocation time and cannot change allocation behaviour. That matters here
// more than usual: an instrument that perturbs a 32 MB budget is measuring
// itself.
//
// What "fragmentation" means on this machine is worth stating, because the
// word usually implies something that does not apply here. Arenas and stacks
// are bump allocators and cannot fragment. Pools hand out fixed blocks and
// cannot fragment either. The only allocator that can is the heap facade, so
// the fragmentation report is about the gap between what the heap has handed
// out and what it has had to reserve to do so.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ps2ur {

class Arena;
class StackAllocator;
class PoolAllocator;

namespace mem {

inline constexpr uint32_t kMaxRegions = 24;

enum class Kind : uint8_t {
    Arena = 0,
    Stack,
    Pool,
    Heap,
    External, // memory the runtime accounts for but does not allocate through
};

struct Region {
    const char* name = "";
    Kind kind = Kind::Arena;
    size_t capacity = 0;
    size_t used = 0;
    size_t peak = 0;
    uint32_t allocs = 0;
    uint32_t failures = 0;
    // Pools only; zero elsewhere.
    size_t block_size = 0;
    size_t blocks_used = 0;
    size_t blocks_total = 0;
};

struct Totals {
    size_t capacity = 0;
    size_t used = 0;
    size_t peak = 0;
    uint32_t allocs = 0;
    uint32_t failures = 0;
};

// A section 15.1 budget line. Matched to a region by name.
struct Budget {
    const char* name;
    size_t bytes;
};

void reset(); // forgets every registration

// Each returns false when the table is full, having logged which name was
// dropped. The pointer must outlive the registration.
bool add_arena(const char* name, const Arena* a);
bool add_stack(const char* name, const StackAllocator* a);
bool add_pool(const char* name, const PoolAllocator* a);
// The heap facade, reported from ps2ur::heap_stats().
bool add_heap(const char* name);
// Memory whose size the runtime knows but does not allocate through an
// allocator: video memory, the managed heap, the loaded ELF. 'used_fn' may be
// null, in which case the region reports 'used' as its fixed capacity.
bool add_external(const char* name, size_t capacity, size_t (*used_fn)(void*),
                  void* ctx);

uint32_t count();
Region snapshot(uint32_t index);
// Snapshot by name; returns false when nothing matches.
bool find(const char* name, Region* out);

Totals totals();

// Bytes the heap has handed out versus bytes it has had to hold to do so,
// as a fraction of the latter. 0 when nothing is outstanding. This is the
// only fragmentation figure on the machine that means anything (see the
// header comment), and it is exact rather than sampled.
float heap_fragmentation();

// Compares each budget line against the matching region's PEAK, not its
// current use: a budget met on average and blown during a load screen is a
// budget that has been blown. Logs one line per breach. Returns the number
// of regions over budget, and 0 when every line fits.
uint32_t check(const Budget* budgets, uint32_t count);

// The memory map screen, as log lines. One row per region with capacity,
// current use, peak and the percentage of capacity the peak reached, then
// the totals. CI parses this, so the prefix [mem] and the column order are
// stable.
void log_map();

// Same content as machine-readable CSV, for the offline viewer.
bool dump_csv(const char* path);

} // namespace mem
} // namespace ps2ur
