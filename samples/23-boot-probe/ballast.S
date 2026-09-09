# samples/23-boot-probe/ballast.S -- the game's load-segment shape.
#
# The boot probe is a 340 KB file that reaches the GS on a console; the game
# is a 5.9 MB file with a 3.5 MB .bss that does not. Whether the console's
# ROM loader delivers a segment that size intact is a question only a segment
# that size can answer, so this is 5.76 MB of initialised data (90 blocks of
# 64 KB, each block a distinct word so a misplaced or truncated block is told
# apart from a missing one) and a 3.4 MB uninitialised array the probe checks
# is zero. Together they put the probe's _end where the game's is.

    .section .rodata
    .globl probe_ballast
    .globl probe_ballast_end
    .balign 16
probe_ballast:
    .set blk, 0
    .rept 90
    .fill 16384, 4, 0xA5000000 + blk
    .set blk, blk + 1
    .endr
probe_ballast_end:

    .section .bss
    .globl probe_bss_ballast
    .balign 64
probe_bss_ballast:
    .space 3565568
