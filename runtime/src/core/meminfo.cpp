// Memory instrumentation implementation (plan section 9, M13 task 2).
#include "ps2ur/meminfo.h"

#include "ps2ur/alloc.h"
#include "ps2ur/log.h"

#include <cstdio>
#include <cstring>

namespace ps2ur {
namespace mem {
namespace {

struct Entry {
    const char* name = "";
    Kind kind = Kind::Arena;
    const void* allocator = nullptr;
    size_t capacity = 0;                       // External only
    size_t (*used_fn)(void*) = nullptr;        // External only
    void* ctx = nullptr;                       // External only
};

Entry g_entries[kMaxRegions];
uint32_t g_count = 0;

bool add(const char* name, Kind kind, const void* allocator)
{
    if (g_count >= kMaxRegions) {
        log(LogLevel::Warn, "mem: registry full (%u); '%s' is not tracked",
            static_cast<unsigned>(kMaxRegions), name);
        return false;
    }
    Entry& e = g_entries[g_count++];
    e = Entry{};
    e.name = name;
    e.kind = kind;
    e.allocator = allocator;
    return true;
}

} // namespace

void reset()
{
    for (uint32_t i = 0; i < kMaxRegions; ++i) {
        g_entries[i] = Entry{};
    }
    g_count = 0;
}

bool add_arena(const char* name, const Arena* a)
{
    return a != nullptr && add(name, Kind::Arena, a);
}

bool add_stack(const char* name, const StackAllocator* a)
{
    return a != nullptr && add(name, Kind::Stack, a);
}

bool add_pool(const char* name, const PoolAllocator* a)
{
    return a != nullptr && add(name, Kind::Pool, a);
}

bool add_heap(const char* name) { return add(name, Kind::Heap, nullptr); }

bool add_external(const char* name, size_t capacity, size_t (*used_fn)(void*),
                  void* ctx)
{
    if (!add(name, Kind::External, nullptr)) {
        return false;
    }
    Entry& e = g_entries[g_count - 1];
    e.capacity = capacity;
    e.used_fn = used_fn;
    e.ctx = ctx;
    return true;
}

uint32_t count() { return g_count; }

Region snapshot(uint32_t index)
{
    Region r;
    if (index >= g_count) {
        return r;
    }
    const Entry& e = g_entries[index];
    r.name = e.name;
    r.kind = e.kind;

    switch (e.kind) {
        case Kind::Arena: {
            const ArenaStats& s = static_cast<const Arena*>(e.allocator)->stats();
            r.capacity = s.capacity;
            r.used = s.used;
            r.peak = s.peak;
            r.allocs = static_cast<uint32_t>(s.alloc_count);
            r.failures = static_cast<uint32_t>(s.fail_count);
            break;
        }
        case Kind::Stack: {
            const ArenaStats& s =
                static_cast<const StackAllocator*>(e.allocator)->stats();
            r.capacity = s.capacity;
            r.used = s.used;
            r.peak = s.peak;
            r.allocs = static_cast<uint32_t>(s.alloc_count);
            r.failures = static_cast<uint32_t>(s.fail_count);
            break;
        }
        case Kind::Pool: {
            const PoolAllocator* p = static_cast<const PoolAllocator*>(e.allocator);
            const ArenaStats& s = p->stats();
            r.capacity = s.capacity;
            r.used = p->used() * p->block_size();
            r.peak = s.peak;
            r.allocs = static_cast<uint32_t>(s.alloc_count);
            r.failures = static_cast<uint32_t>(s.fail_count);
            r.block_size = p->block_size();
            r.blocks_used = p->used();
            r.blocks_total = p->capacity();
            break;
        }
        case Kind::Heap: {
            const HeapStats& s = heap_stats();
            r.capacity = s.budget;
            r.used = s.outstanding;
            r.peak = s.peak;
            r.allocs = s.allocs;
            r.failures = s.failures;
            break;
        }
        case Kind::External: {
            r.capacity = e.capacity;
            r.used = e.used_fn != nullptr ? e.used_fn(e.ctx) : e.capacity;
            r.peak = r.used;
            break;
        }
    }
    return r;
}

bool find(const char* name, Region* out)
{
    if (name == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < g_count; ++i) {
        if (std::strcmp(g_entries[i].name, name) == 0) {
            if (out != nullptr) {
                *out = snapshot(i);
            }
            return true;
        }
    }
    return false;
}

Totals totals()
{
    Totals t;
    for (uint32_t i = 0; i < g_count; ++i) {
        const Region r = snapshot(i);
        t.capacity += r.capacity;
        t.used += r.used;
        t.peak += r.peak;
        t.allocs += r.allocs;
        t.failures += r.failures;
    }
    return t;
}

float heap_fragmentation()
{
    const HeapStats& s = heap_stats();
    if (s.peak == 0) {
        return 0.0f;
    }
    // Outstanding against the high-water mark: the fraction of the peak
    // footprint that is currently not doing any work. A steadily rising
    // number across a soak is the signature of a leak or of churn the
    // allocator cannot hand back.
    const float out = static_cast<float>(s.outstanding);
    const float peak = static_cast<float>(s.peak);
    const float idle = peak - out;
    return idle <= 0.0f ? 0.0f : idle / peak;
}

uint32_t check(const Budget* budgets, uint32_t budget_count)
{
    uint32_t over = 0;
    for (uint32_t i = 0; i < budget_count; ++i) {
        Region r;
        if (!find(budgets[i].name, &r)) {
            log(LogLevel::Warn, "[mem] budget '%s' has no matching region",
                budgets[i].name);
            continue;
        }
        // Peak, not current: a budget blown only during a load is blown.
        if (r.peak > budgets[i].bytes) {
            over++;
            log(LogLevel::Error,
                "[mem] OVER BUDGET %s: peak %u KB exceeds %u KB by %u KB",
                r.name, static_cast<unsigned>(r.peak / 1024),
                static_cast<unsigned>(budgets[i].bytes / 1024),
                static_cast<unsigned>((r.peak - budgets[i].bytes + 1023) / 1024));
        } else {
            log(LogLevel::Info, "[mem] budget %s: peak %u of %u KB (%u%%)",
                r.name, static_cast<unsigned>(r.peak / 1024),
                static_cast<unsigned>(budgets[i].bytes / 1024),
                budgets[i].bytes == 0
                    ? 0u
                    : static_cast<unsigned>(r.peak * 100 / budgets[i].bytes));
        }
    }
    return over;
}

void log_map()
{
    log(LogLevel::Info, "[mem] %-18s %10s %10s %10s %5s", "region", "capacity",
        "used", "peak", "peak%");
    for (uint32_t i = 0; i < g_count; ++i) {
        const Region r = snapshot(i);
        const unsigned pct =
            r.capacity == 0 ? 0u
                            : static_cast<unsigned>(r.peak * 100 / r.capacity);
        log(LogLevel::Info, "[mem] %-18s %9u K %9u K %9u K %4u%%", r.name,
            static_cast<unsigned>(r.capacity / 1024),
            static_cast<unsigned>(r.used / 1024),
            static_cast<unsigned>(r.peak / 1024), pct);
        if (r.kind == Kind::Pool) {
            log(LogLevel::Info, "[mem]   blocks %u of %u at %u bytes",
                static_cast<unsigned>(r.blocks_used),
                static_cast<unsigned>(r.blocks_total),
                static_cast<unsigned>(r.block_size));
        }
    }
    const Totals t = totals();
    log(LogLevel::Info, "[mem] TOTAL capacity %u K used %u K peak %u K",
        static_cast<unsigned>(t.capacity / 1024),
        static_cast<unsigned>(t.used / 1024),
        static_cast<unsigned>(t.peak / 1024));
    log(LogLevel::Info, "[mem] allocations %u failures %u heap-idle %.1f%%",
        static_cast<unsigned>(t.allocs), static_cast<unsigned>(t.failures),
        static_cast<double>(heap_fragmentation() * 100.0f));
}

bool dump_csv(const char* path)
{
    FILE* f = std::fopen(path, "w");
    if (f == nullptr) {
        log(LogLevel::Error, "mem: cannot open '%s' for the CSV dump", path);
        return false;
    }
    std::fprintf(f, "region,kind,capacity,used,peak,allocs,failures,"
                    "block_size,blocks_used,blocks_total\n");
    static const char* kKindNames[] = {"arena", "stack", "pool", "heap",
                                       "external"};
    for (uint32_t i = 0; i < g_count; ++i) {
        const Region r = snapshot(i);
        std::fprintf(f, "%s,%s,%u,%u,%u,%u,%u,%u,%u,%u\n", r.name,
                     kKindNames[static_cast<uint32_t>(r.kind)],
                     static_cast<unsigned>(r.capacity),
                     static_cast<unsigned>(r.used),
                     static_cast<unsigned>(r.peak),
                     static_cast<unsigned>(r.allocs),
                     static_cast<unsigned>(r.failures),
                     static_cast<unsigned>(r.block_size),
                     static_cast<unsigned>(r.blocks_used),
                     static_cast<unsigned>(r.blocks_total));
    }
    std::fclose(f);
    log(LogLevel::Info, "mem: wrote %u regions to %s",
        static_cast<unsigned>(g_count), path);
    return true;
}

} // namespace mem
} // namespace ps2ur
