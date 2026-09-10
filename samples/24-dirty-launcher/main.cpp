// samples/24-dirty-launcher -- a launcher that leaves memory the way a real
// one does.
//
// PCSX2 boots from zeroed RAM. A console launched from uLaunchELF or Open
// PS2 Loader hands the game a heap and a stack full of whatever the launcher
// and the kernel left behind, and the game's libc start-up hangs on the
// console while it sails through in the emulator (docs/hardware-bring-up.md,
// second day). This ELF links high in RAM, fills every byte it does not
// occupy with a pattern that is not zero, loads the game through the same
// ROM loader a launcher uses, and jumps to it. If the game then hangs in
// PCSX2, the difference was the memory, and the emulator can say where.
//
// Usage: stage the target next to this ELF as game.elf; the harness does that
// with EMU_EXTRA_FILES. The pattern is 0xA5, chosen so that any field a
// program reads without writing first is neither zero nor a plausible
// pointer.
#include <kernel.h>
#include <loadfile.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>

extern "C" char _ftext[];
extern "C" char _end[];

namespace {

constexpr unsigned kUserStart = 0x00100000u;
constexpr unsigned kRamEnd = 0x02000000u;

void fill(unsigned from, unsigned to)
{
    memset(reinterpret_cast<void*>(from), 0xA5, to - from);
}

} // namespace

// A private stack inside this ELF's own image, so that the top of RAM, where
// the kernel puts every program's stack and where the game's libc start-up
// frames will sit, can be dirtied too.
alignas(16) char g_alt_stack[16384];

[[noreturn]] __attribute__((noinline)) void stage2()
{
    // Everything below this ELF, and everything above it to the end of RAM.
    const unsigned self_start = reinterpret_cast<unsigned>(_ftext) & ~0xFFFu;
    const unsigned self_end = (reinterpret_cast<unsigned>(_end) + 0xFFFu) & ~0xFFFu;
    fill(kUserStart, self_start);
    fill(self_end, kRamEnd);
    FlushCache(0);
    printf("[dirty-launcher] filled %08x-%08x and %08x-%08x, loading game.elf\n",
           kUserStart, self_start, self_end, kRamEnd);

    t_ExecData exec;
    memset(&exec, 0, sizeof(exec));
    const int rc = SifLoadElf("host:game.elf", &exec);
    if (rc != 0 || exec.epc == 0) {
        printf("[dirty-launcher] SifLoadElf failed (%d)\n", rc);
        printf("PS2UR_TOKEN_DIRTY_FAIL load\n");
        SleepThread();
        for (;;) {
        }
    }
    printf("[dirty-launcher] entry %08x gp %08x, jumping\n",
           static_cast<unsigned>(exec.epc), static_cast<unsigned>(exec.gp));
    FlushCache(0);
    FlushCache(2);
    static char* args[] = {const_cast<char*>("host:game.elf")};
    ExecPS2(reinterpret_cast<void*>(exec.epc), reinterpret_cast<void*>(exec.gp), 1,
            args);
    SleepThread();
    for (;;) {
    }
}

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;
    SifInitRpc(0);
    SifLoadFileInit();
    // Move off the kernel-provided stack before touching the top of RAM.
    // stage2 never returns, so nothing of this frame is needed again.
    __asm__ __volatile__("move $sp, %0" : : "r"(g_alt_stack + sizeof(g_alt_stack) - 32)
                         : "memory");
    stage2();
}

// The program headers must not start below the region the game loads into.
// See CMakeLists.txt: this ELF links at 0x01F00000 (kRamEnd - 1 MB).
static_assert(kRamEnd > kUserStart, "layout");
