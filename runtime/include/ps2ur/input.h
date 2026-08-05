// input: DualShock 2 pads on both ports -- digital buttons, analog sticks
// with a deadzone, pressure-sensitive buttons and rumble (plan section 9,
// M10 task 2).
//
// This is the native layer; the Unity-shaped `Input` facade and the
// `PS2Input` extras (pressure, rumble) sit on top of it in the managed shim.
//
// Two libpad details worth knowing before reading the implementation:
//   - The button word is ACTIVE LOW: a bit is 0 while its button is held.
//   - A pad takes several frames to reach PAD_STATE_STABLE after the port
//     opens, and analog/pressure modes have to be requested explicitly.
#pragma once

#include <cstdint>

namespace ps2ur {
namespace input {

inline constexpr uint32_t kMaxPorts = 2;

// Ordered to match libpad's bit layout so the mapping is a shift, not a
// switch.
enum class Button : uint32_t {
    Select = 0,
    L3,
    R3,
    Start,
    Up,
    Right,
    Down,
    Left,
    L2,
    R2,
    L1,
    R1,
    Triangle,
    Circle,
    Cross,
    Square,
    Count,
};

bool init();
void shutdown();
bool initialized();

// Polls every port. Call once per frame, before reading any state: the edge
// queries below compare against the previous call.
void update();

// Recorded input playback (plan section 9, M13 task 5: "30-minute automated
// play session (recorded input playback)").
//
// A soak test has to press buttons for half an hour without a person in the
// room, and it has to press the SAME buttons every run or a failure is not
// reproducible. update() substitutes the recorded frame for the hardware
// poll while playback is active, so everything above this line behaves
// exactly as it does with a pad in someone's hands, including the edge
// queries.
namespace playback {

// One recorded frame. Buttons are a bitmask indexed by Button; sticks are
// raw 0..255 pad bytes, so a recording is byte-identical to what the pad
// reported and needs no rescaling on the way back in.
struct Frame {
    uint16_t buttons = 0;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
};

// Drives port 0 from 'frames'. The table is not copied, so it must outlive
// playback. When the end is reached it loops, which is what turns a ten
// second recording into a thirty minute session.
void start(const Frame* frames, uint32_t count);
void stop();
bool active();
uint32_t position(); // index of the frame update() will apply next
uint32_t loops();    // times the recording has wrapped

} // namespace playback

bool connected(uint32_t port);
// True once the pad reports the DualShock 2 mode that carries sticks and
// pressure. A digital-only pad stays false and reports centred sticks.
bool analog_available(uint32_t port);
bool pressure_available(uint32_t port);
bool rumble_available(uint32_t port);

bool button(uint32_t port, Button button);      // held now
bool button_down(uint32_t port, Button button); // went down this frame
bool button_up(uint32_t port, Button button);   // came up this frame
uint8_t pressure(uint32_t port, Button button); // 0..255, 0 when unsupported

// Sticks in -1..1 with the deadzone applied. Y is POSITIVE UP, matching
// Unity's axis convention rather than the pad's raw byte (which grows
// downward).
float axis_x(uint32_t port, bool right_stick);
float axis_y(uint32_t port, bool right_stick);

// Raw 0..255 stick bytes, for calibration screens and tests.
uint8_t raw_axis(uint32_t port, bool right_stick, bool vertical);

void set_deadzone(float deadzone);
float deadzone();

// Rumble. The small motor is on/off; the large motor takes 0..255.
void set_rumble(uint32_t port, bool small_motor, uint8_t large_motor);

// The deadzone curve on its own: raw byte (128 is centre) to -1..1,
// rescaled so the axis still reaches +-1 at the extremes rather than
// jumping when it leaves the deadzone. Pure maths, tested on the host.
float apply_deadzone(uint8_t raw, float deadzone);

// Feeds a synthetic pad frame, for tests and for replaying recorded input.
// Real polling overwrites it on the next update(); with no pad connected it
// is the only source of state, which is what makes button and axis handling
// testable without hardware.
void inject_frame(uint32_t port, uint16_t buttons_active_low, uint8_t lx,
                  uint8_t ly, uint8_t rx, uint8_t ry,
                  const uint8_t* pressures /* 16 entries, or nullptr */);

} // namespace input
} // namespace ps2ur
