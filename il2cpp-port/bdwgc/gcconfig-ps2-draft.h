/*
 * gcconfig-ps2-draft.h
 *
 * DRAFT machine/OS configuration stanza for porting bdwgc (the
 * Boehm-Demers-Weiser conservative collector shipped with Unity IL2CPP at
 * Editor/Data/il2cpp/external/bdwgc) to the PlayStation 2 Emotion Engine
 * (R5900, MIPS III subset, little-endian, 32 MB RDRAM -- plan section 3.1).
 *
 * STATUS: standalone reference only. Written from scratch (no Unity-owned
 * text); not included in any build. It becomes a patch against the staged
 * build/il2cpp/bdwgc/include/private/gcconfig.h (inserted into its MIPS
 * dispatch section) via il2cpp-port/patches/README.md once the IL2CPP port
 * spec lands. TODO(spec missing: section 11)
 *
 * ADR-004 (plan section 5) recap:
 *   - v1: port bdwgc; it needs only data-segment bounds, stack bottom,
 *     alignment, single-thread config -- i.e. exactly this stanza.
 *   - M6 bring-up alternative: null GC (allocate, never collect) to isolate
 *     GC issues. TODO(spec missing: section 9) for the M6 gate.
 *   - M10 revisit: precise mark-sweep over a fixed arena if bdwgc
 *     stop-the-world pauses exceed 6 ms on the 294 MHz EE.
 */

#ifndef GCCONFIG_PS2_DRAFT_H
#define GCCONFIG_PS2_DRAFT_H

/* ---- machine recognition -------------------------------------------- */
/* In the real gcconfig.h this hangs off the MIPS cpu dispatch. The ps2dev */
/* compiler (mips64r5900el-ps2-elf-gcc) predefines __mips__ and an R5900   */
/* marker. [TODO verify] the exact predefines with                         */
/*   mips64r5900el-ps2-elf-gcc -dM -E - </dev/null                         */
/* once the toolchain install at %PS2DEV% (C:/Users/Ash/ps2dev) completes; */
/* candidates: _R5900, __R5900, __mips64. The build can also simply pass   */
/* -DGC_PS2 and key on that, which is version-proof.                       */
#define MIPS              /* cpu family bucket gcconfig.h already knows    */
#define mach_type_known   /* suppress gcconfig.h's "unknown machine" error */

/* ---- OS bucket ------------------------------------------------------- */
/* Bare-metal newlib-on-ps2sdk; no existing gcconfig OS bucket fits, so we */
/* introduce our own tag. Alternative when writing the real patch: reuse   */
/* gcconfig.h's existing embedded/no-OS ("NOSYS"-style) pattern used by    */
/* other bare-metal targets. TODO(spec missing: section 11)                */
#define OS_TYPE "PS2"

/* ---- word size and alignment ----------------------------------------- */
/* EE GPRs are 128-bit but the ps2dev ABI uses 32-bit pointers and a       */
/* 32-bit address space (plan 3.1), so this is a 32-bit collector build.   */
#define CPP_WORDSZ 32
/* ALIGNMENT: minimum alignment (bytes) of pointers the scanner assumes,   */
/* and of objects the allocator returns. Bare pointers only need 4, but    */
/* C 'double' fields and 64-bit ld/sd accesses need 8, so 8 is the safe    */
/* draft value ([VERIFY] flagged in plan convention). [TODO verify]        */
/* whether GC-allocated objects are ever touched with 128-bit lq/sq        */
/* (quadword copies of managed structs by IL2CPP-generated code or ps2ur); */
/* lq/sq trap on <16-byte alignment -- if so, bump ALIGNMENT to 16 and     */
/* accept the fragmentation cost.                                          */
#define ALIGNMENT 8

/* ---- static root bounds ---------------------------------------------- */
/* A conservative GC must scan every static region that can hold pointers  */
/* into the managed heap: .data, .sdata (empty under the standard -G0      */
/* flag, plan 3.1 ABI note) and .bss. .rodata is excluded: link-time       */
/* constants cannot point at runtime heap objects.                         */
/* The symbols below are placeholders for whatever the ps2sdk EE linker    */
/* script really defines. [TODO verify] against                            */
/* $PS2DEV/ps2sdk/ee/startup/linkfile -- candidates: _fdata/_edata,        */
/* __bss_start/_end, _fbss/_end. Draft assumption: one contiguous root     */
/* range from start-of-.data to end-of-.bss.                               */
extern int _fdata[];   /* start of .data          [TODO verify symbol]     */
extern int _end[];     /* end of .bss (heap base) [TODO verify symbol]     */
#define DATASTART ((ptr_t)_fdata)
#define DATAEND   ((ptr_t)_end)

/* ---- main-thread stack bottom ---------------------------------------- */
/* EE RAM spans 0x00000000..0x02000000 (32 MB). ps2sdk crt0 places the     */
/* initial stack at the top of RAM, growing down; the GC scans from the    */
/* current SP up to STACKBOTTOM. 0x02000000 (end of RAM) is the draft      */
/* over-approximation. [TODO verify] against crt0's _stack/_stack_size     */
/* (crt0 reserves argument space below the top, and debug environments     */
/* like ps2link occupy the top of RAM). If ps2ur relocates the main stack  */
/* into its own arena, replace this constant with a runtime assignment of  */
/* GC_stackbottom during ps2ur::Init.                                      */
#define STACKBOTTOM ((ptr_t)0x02000000)
/* MIPS stacks grow downward: do NOT define STACK_GROWS_UP (gcconfig       */
/* default is downward). No HEURISTIC stack probing -- the bound is fixed. */

/* ---- threading: single-threaded first (plan os/ps2 strategy) ---------- */
/* Do NOT define GC_THREADS / THREADS / PARALLEL_MARK / THREAD_LOCAL_ALLOC */
/* anywhere in the build. One EE thread runs everything, so world-stop is  */
/* implicit and free. Revisiting ps2sdk CreateThread later would require   */
/* per-thread stack registration and a stop-the-world mechanism, neither   */
/* of which ps2sdk provides out of the box.                                */

/* ---- OS services the PS2 does not have -------------------------------- */
#define NO_GETENV               /* no environment variables: GCSettings /  */
                                /* GC_* env tuning knobs become            */
                                /* compile-time choices only               */
#define GC_DISABLE_INCREMENTAL  /* no mprotect/dirty-bit write barrier on  */
                                /* bare metal: full-heap stop-the-world    */
                                /* collections only                        */
/* DYNAMIC_LOADING deliberately NOT defined: a PS2 ELF is a single static  */
/* image; there are no dynamic libraries to find roots in.                 */
/* MPROTECT_VDB / USE_MMAP / USE_MUNMAP deliberately NOT defined: no       */
/* virtual-memory services.                                                */
#define GETPAGESIZE() 4096      /* only feeds GC heap-growth granularity;  */
                                /* EE TLB minimum page is 4 KB             */
                                /* [TODO verify]                           */

/* ---- heap acquisition -------------------------------------------------- */
/* No sbrk/mmap. Heap expansion must come from a fixed arena owned by the  */
/* ps2ur allocator (plan 3.6 budget: 4 MB managed heap, 6 MB stretch;      */
/* D4 caps total RAM at 30 MB). The real patch will define, roughly:       */
/*   #define GET_MEM(bytes) ps2ur_gc_get_mem(bytes)                        */
/* with an extern "C" hook returning page-aligned, zeroed memory or NULL   */
/* when the arena is exhausted (which must fail allocation, not grow).     */
/* Hook name/ownership: TODO(spec missing: section 11).                    */

/* ---- knobs to evaluate at the M10 pause-time gate ---------------------- */
/* SMALL_CONFIG            -- shrinks collector tables/features; likely    */
/*                            appropriate for a 32 MB machine.             */
/* GC_free_space_divisor   -- raise to trade throughput for smaller heap.  */
/* Initial/max heap size   -- set at runtime in ps2ur::Init via GC API,    */
/*                            not here.                                    */

#endif /* GCCONFIG_PS2_DRAFT_H */
