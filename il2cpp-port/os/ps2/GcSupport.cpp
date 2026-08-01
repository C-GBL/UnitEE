// bdwgc platform hooks for the PS2 stanza in gcconfig.h (plan M6 task 4).
//
// Two symbols, both referenced from the gcconfig PS2 block:
//  - ps2ur_gc_get_mem: heap expansion. bdwgc grows in HBLKSIZE-aligned
//    chunks; newlib memalign carves them from the flat 32 MB RDRAM heap.
//    Chunks are never returned (bdwgc keeps its heap; no USE_MUNMAP).
//  - ps2ur_gc_stack_bottom: conservative stack scanning needs the address
//    the main stack grows down from. The BIOS chooses the initial sp at
//    load time, so main() captures it before il2cpp_init/GC_INIT runs.
//    The fallback constant is the top of RDRAM: correct direction, wider
//    scan than necessary, never wrong-side.
#include "il2cpp-config.h"

#if IL2CPP_TARGET_PS2

#include <stddef.h>
#include <malloc.h>

extern "C"
{
    char* ps2ur_gc_stack_bottom = reinterpret_cast<char*>(0x02000000);

    void* ps2ur_gc_get_mem(size_t lb)
    {
        const size_t kBlock = 4096; // HBLKSIZE
        lb = (lb + kBlock - 1) & ~(kBlock - 1);
        return memalign(kBlock, lb);
    }
}

#endif // IL2CPP_TARGET_PS2
