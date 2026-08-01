/* Supplement to the ps2sdk linkfile for --gc-sections builds (M6).
 *
 * The stock linkfile predates exception-heavy binaries: .eh_frame and
 * .gcc_except_table are orphan sections there, and section GC breaks the
 * FDE/LSDA chains -- observed as the first managed throw hanging the EE
 * (docs/notes/verify-log.md, M6). Pinning both keeps the unwinder's data
 * intact while code GC still runs.
 */
SECTIONS
{
    .eh_frame : {
        KEEP(*(.eh_frame))
        KEEP(*(.eh_frame.*))
    }
    .gcc_except_table : {
        KEEP(*(.gcc_except_table))
        KEEP(*(.gcc_except_table.*))
    }
} INSERT AFTER .dtors;
