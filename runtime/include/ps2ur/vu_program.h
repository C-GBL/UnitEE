// VU1 microprogram management (plan section 9, M4 task 1).
//
// VU1 has 16 KB of micro memory and 16 KB of data memory, and is the only unit
// with a direct path to the GS (PATH1). A microprogram is assembled by dvp-as
// into the ELF as a blob delimited by <Name>_CodeStart / <Name>_CodeEnd, DMA'd
// into micro memory via VIF1's MPG command, and started with MSCAL.
//
// The VIF/DMA plumbing goes through ps2sdk's packet2 helpers rather than our
// own packet builder. That is deliberate: M4 is the highest-risk milestone in
// the plan (section 16, R4), and the risk that matters is in the microprograms
// themselves, not in re-deriving a VIF command encoder that ps2sdk already
// ships working. Our own GIF packet path stays ours because that is the one
// the renderer manipulates every frame.
#pragma once

#include <cstdint>

namespace ps2ur {
namespace vu {

// Micro memory is addressed in 64-bit instruction slots; 16 KB = 2048 of them.
inline constexpr uint32_t kMicroMemInstructions = 2048;
// Data memory is addressed in quadwords; 16 KB = 1024 of them.
inline constexpr uint32_t kDataMemQwords = 1024;

class MicroProgram {
public:
    // 'start' and 'end' are the linker symbols dvp-as emitted, e.g.
    //   extern "C" u32 VuUnlit_CodeStart __attribute__((section(".vudata")));
    // 'vu_address' is where in micro memory to place it, in instruction slots.
    void set_blob(const void* start, const void* end, uint32_t vu_address = 0);

    // Uploads the blob to VU1 micro memory. Safe to call once at load time;
    // re-uploading every frame wastes bandwidth for no benefit, which is why
    // uploaded() exists.
    bool upload();

    // Runs the program from its entry offset. Data must already be in VU1
    // data memory (see unpack_data).
    bool start(uint32_t entry_offset = 0);

    // Copies quadwords into VU1 data memory at 'dest_qword'. This is the VIF
    // UNPACK path; at M4 task 6 the real renderer replaces per-call unpacks
    // with a pre-built DMA chain.
    bool unpack_data(const void* qwords, uint32_t count, uint32_t dest_qword);

    // Instruction slots the blob occupies.
    uint32_t size_instructions() const { return m_size_instructions; }
    uint32_t vu_address() const { return m_vu_address; }
    bool uploaded() const { return m_uploaded; }

private:
    const void* m_start = nullptr;
    const void* m_end = nullptr;
    uint32_t m_vu_address = 0;
    uint32_t m_size_instructions = 0;
    bool m_uploaded = false;
};

// Waits for VU1 to finish the program it is running. Blocking; the shipping
// renderer overlaps EE and VU1 work instead (plan section 15.3 budgets the EE
// as idle at the end of a frame, not spinning here).
void wait_idle();

} // namespace vu
} // namespace ps2ur
