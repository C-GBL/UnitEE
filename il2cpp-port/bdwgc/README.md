# il2cpp-port/bdwgc -- GC port reference (ADR-004)

ADR-004 (plan section 5) chose **porting bdwgc** -- the conservative
collector IL2CPP already uses, shipped with the Editor at
`il2cpp/external/bdwgc` -- for v1, because it needs only: data-segment
bounds, a stack bottom, an alignment, and a single-thread config. That is
a `gcconfig.h` stanza, not a rewrite.

## Contents

- `gcconfig-ps2-draft.h` -- the draft stanza for a PS2/EE (R5900) machine
  block, written from scratch as a standalone, fully commented reference.
  It is NOT compiled by anything and is NOT a patch yet.

## How it becomes real

Once the IL2CPP port spec exists, the stanza is folded into the staged
`build/il2cpp/bdwgc/include/private/gcconfig.h` (MIPS dispatch section)
and captured as `patches/NNN-bdwgc-ps2-gcconfig.patch` using the workflow
in `../patches/README.md`. TODO(spec missing: section 11)

## ADR-004 alternatives to keep alive

- **Null GC** (allocate, never collect): the M6 bring-up step, to isolate
  GC issues from everything else. TODO(spec missing: section 9) for the
  exact M6 gate criteria.
- **Precise mark-sweep over a fixed arena**: revisit at M10 if bdwgc
  stop-the-world pauses exceed **6 ms** on the 294 MHz EE (33 ms frame,
  plan section 3.6 budgets 4-6 MB managed heap).

## Open [TODO verify] items (marked inline in the draft)

- `ALIGNMENT 8` vs 16 (128-bit `lq`/`sq` traps on under-aligned access)
- static-root linker symbols (`_fdata`/`_end` vs `__bss_start`/`_fbss`)
  against the real `$PS2DEV/ps2sdk/ee/startup/linkfile`
- `STACKBOTTOM` vs the crt0 `_stack`/`_stack_size` placement near the
  0x02000000 end of RAM
- EE page size assumption behind `GETPAGESIZE()`
- exact compiler predefines of `mips64r5900el-ps2-elf-gcc` for the
  machine-recognition `#if` (toolchain still installing; do not check yet)
