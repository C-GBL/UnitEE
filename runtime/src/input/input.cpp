#include "ps2ur/input.h"

#include "ps2ur/log.h"
#include "ps2ur/platform.h"

#if defined(PS2UR_PLATFORM_PS2)
#include <libpad.h>

extern "C" {
extern unsigned char sio2man_irx_start[];
extern unsigned char sio2man_irx_end[];
extern unsigned char padman_irx_start[];
extern unsigned char padman_irx_end[];
}
#endif

namespace ps2ur {
namespace input {

namespace {

bool g_initialized = false;
float g_deadzone = 0.2f;

struct PadState {
    bool connected = false;
    bool analog = false;
    bool pressure = false;
    bool rumble = false;
    // ACTIVE HIGH here: the driver's active-low word is inverted on read, so
    // everything above this layer sees "1 means pressed".
    uint16_t buttons = 0;
    uint16_t previous = 0;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    uint8_t pressures[16] = {};
    uint32_t settle_frames = 0;
    // The DualShock capability handshake, one asynchronous request at a
    // time: 0 request analog, 1 verify it, 2 request pressure, 3 confirm,
    // 4 request actuator alignment, 5 confirm, 6 configured. Reset on
    // disconnect so a re-plugged pad negotiates again.
    uint8_t config_stage = 0;
};

PadState g_pads[kMaxPorts];

#if defined(PS2UR_PLATFORM_PS2)
// libpad wants a 256-byte buffer per port, 64-byte aligned.
alignas(64) uint8_t g_pad_buffer[kMaxPorts][256];
// Actuator alignment: slot 0 drives the small motor, slot 1 the large one.
uint8_t g_act_align[kMaxPorts][6];

// libpad's pressure bytes arrive in a fixed order; map them onto our button
// enum so callers never see the driver's layout.
constexpr int kPressureIndex[16] = {
    -1, -1, -1, -1, // Select, L3, R3, Start have no pressure
    0,              // Up
    1,              // Right
    2,              // Down
    3,              // Left
    4,              // L2
    5,              // R2
    6,              // L1
    7,              // R1
    8,              // Triangle
    9,              // Circle
    10,             // Cross
    11,             // Square
};
#endif

float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

float apply_deadzone(uint8_t raw, float deadzone)
{
    // 128 is centre. The negative side spans 128 counts and the positive
    // side 127, so normalise each half separately or a centred stick reads
    // slightly positive.
    const float value = raw >= 128 ? (raw - 128) / 127.0f
                                   : (raw - 128) / 128.0f;
    const float magnitude = value < 0.0f ? -value : value;
    if (magnitude <= deadzone) {
        return 0.0f;
    }
    // Rescale so the axis still reaches +-1 at the extremes; without this
    // the stick jumps to 'deadzone' the moment it leaves the dead area.
    const float span = 1.0f - deadzone;
    const float scaled = (magnitude - deadzone) / (span > 0.0f ? span : 1.0f);
    return clampf(value < 0.0f ? -scaled : scaled, -1.0f, 1.0f);
}

bool init()
{
    if (g_initialized) {
        return true;
    }
    for (uint32_t i = 0; i < kMaxPorts; ++i) {
        g_pads[i] = PadState{};
    }
#if defined(PS2UR_PLATFORM_PS2)
    const int sio2 = platform::load_irx(
        "freesio2.irx", sio2man_irx_start,
        static_cast<unsigned>(sio2man_irx_end - sio2man_irx_start));
    const int pad = platform::load_irx(
        "freepad.irx", padman_irx_start,
        static_cast<unsigned>(padman_irx_end - padman_irx_start));
    if (sio2 < 0 || pad < 0) {
        log(LogLevel::Error, "input: IOP modules failed (sio2=%d pad=%d)", sio2,
            pad);
        return false;
    }
    // padInit returns 1 on success, NOT 0 -- unlike most of ps2sdk.
    if (padInit(0) != 1) {
        log(LogLevel::Error, "input: padInit failed");
        return false;
    }
    for (uint32_t port = 0; port < kMaxPorts; ++port) {
        if (padPortOpen(static_cast<int>(port), 0, g_pad_buffer[port]) == 0) {
            log(LogLevel::Warn, "input: port %u would not open",
                static_cast<unsigned>(port));
        }
    }
#endif
    g_initialized = true;
    log(LogLevel::Debug, "input: init, %u ports",
        static_cast<unsigned>(kMaxPorts));
    return true;
}

void shutdown()
{
    if (!g_initialized) {
        return;
    }
#if defined(PS2UR_PLATFORM_PS2)
    for (uint32_t port = 0; port < kMaxPorts; ++port) {
        padPortClose(static_cast<int>(port), 0);
    }
    padEnd();
#endif
    g_initialized = false;
}

bool initialized() { return g_initialized; }

void update()
{
    for (uint32_t port = 0; port < kMaxPorts; ++port) {
        g_pads[port].previous = g_pads[port].buttons;
    }
#if defined(PS2UR_PLATFORM_PS2)
    for (uint32_t port = 0; port < kMaxPorts; ++port) {
        PadState& pad = g_pads[port];
        const int state = padGetState(static_cast<int>(port), 0);
        if (state == PAD_STATE_DISCONN) {
            pad = PadState{};
            continue;
        }
        if (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1) {
            // Still negotiating; hold the last known state rather than
            // reporting a burst of phantom releases.
            continue;
        }
        if (!pad.connected) {
            pad.connected = true;
            pad.settle_frames = 0;
        }

        // The DualShock capability handshake. Every pad command here is
        // ASYNCHRONOUS: issuing one and immediately reading the result sees
        // the state from BEFORE the command ran. The first version of this
        // re-issued padSetMainMode on every retry and then asked for the
        // mode -- so the answer always described a request still in flight,
        // the loop never settled, and the emulator logged a config sequence
        // five times a second for 24 seconds (verify-log M12.5). One
        // request at a time, verified only after the driver reports it
        // complete.
        if (pad.config_stage < 6u && pad.settle_frames < 240u) {
            ++pad.settle_frames;
            const int rstat = padGetReqState(static_cast<int>(port), 0);
            if (rstat == PAD_RSTAT_FAILED &&
                (pad.config_stage == 1u || pad.config_stage == 3u ||
                 pad.config_stage == 5u)) {
                --pad.config_stage; // re-issue the request that failed
            } else if (rstat == PAD_RSTAT_COMPLETE &&
                       padGetState(static_cast<int>(port), 0) ==
                           PAD_STATE_STABLE) {
                switch (pad.config_stage) {
                    case 0: // ask for analog mode, locked
                        padSetMainMode(static_cast<int>(port), 0,
                                       PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
                        pad.config_stage = 1;
                        break;
                    case 1: // did it take? (a digital pad never will; the
                            // frame budget gives up on those quietly)
                        if (padInfoMode(static_cast<int>(port), 0,
                                        PAD_MODECURID, 0) ==
                            PAD_TYPE_DUALSHOCK) {
                            pad.analog = true;
                            pad.config_stage = 2;
                        }
                        break;
                    case 2: // pressure-sensitive buttons
                        if (padInfoPressMode(static_cast<int>(port), 0) != 0) {
                            padEnterPressMode(static_cast<int>(port), 0);
                            pad.config_stage = 3;
                        } else {
                            pad.config_stage = 4;
                        }
                        break;
                    case 3:
                        pad.pressure = true;
                        pad.config_stage = 4;
                        break;
                    case 4: // rumble actuators
                        if (padInfoAct(static_cast<int>(port), 0, -1, 0) > 0) {
                            g_act_align[port][0] = 0; // small motor
                            g_act_align[port][1] = 1; // large motor
                            for (int i = 2; i < 6; ++i) {
                                g_act_align[port][i] = 0xFF;
                            }
                            padSetActAlign(
                                static_cast<int>(port), 0,
                                reinterpret_cast<char*>(g_act_align[port]));
                            pad.config_stage = 5;
                        } else {
                            pad.config_stage = 6;
                        }
                        break;
                    default:
                        pad.rumble = true;
                        pad.config_stage = 6;
                        break;
                }
            }
        }

        padButtonStatus status;
        if (padRead(static_cast<int>(port), 0, &status) != 0) {
            // ACTIVE LOW from the driver: invert once, here.
            pad.buttons = static_cast<uint16_t>(0xFFFFu ^ status.btns);
            pad.lx = status.ljoy_h;
            pad.ly = status.ljoy_v;
            pad.rx = status.rjoy_h;
            pad.ry = status.rjoy_v;
            if (pad.pressure) {
                const uint8_t* src = &status.right_p;
                for (int i = 0; i < 12; ++i) {
                    pad.pressures[i] = src[i];
                }
            }
        }
    }
#endif
}

bool connected(uint32_t port)
{
    return port < kMaxPorts && g_pads[port].connected;
}

bool analog_available(uint32_t port)
{
    return port < kMaxPorts && g_pads[port].analog;
}

bool pressure_available(uint32_t port)
{
    return port < kMaxPorts && g_pads[port].pressure;
}

bool rumble_available(uint32_t port)
{
    return port < kMaxPorts && g_pads[port].rumble;
}

bool button(uint32_t port, Button b)
{
    if (port >= kMaxPorts || b >= Button::Count) {
        return false;
    }
    return (g_pads[port].buttons & (1u << static_cast<uint32_t>(b))) != 0u;
}

bool button_down(uint32_t port, Button b)
{
    if (port >= kMaxPorts || b >= Button::Count) {
        return false;
    }
    const uint16_t mask = static_cast<uint16_t>(1u << static_cast<uint32_t>(b));
    return (g_pads[port].buttons & mask) != 0u &&
           (g_pads[port].previous & mask) == 0u;
}

bool button_up(uint32_t port, Button b)
{
    if (port >= kMaxPorts || b >= Button::Count) {
        return false;
    }
    const uint16_t mask = static_cast<uint16_t>(1u << static_cast<uint32_t>(b));
    return (g_pads[port].buttons & mask) == 0u &&
           (g_pads[port].previous & mask) != 0u;
}

uint8_t pressure(uint32_t port, Button b)
{
    if (port >= kMaxPorts || b >= Button::Count) {
        return 0;
    }
    return g_pads[port].pressures[static_cast<uint32_t>(b)];
}

uint8_t raw_axis(uint32_t port, bool right_stick, bool vertical)
{
    if (port >= kMaxPorts) {
        return 128;
    }
    const PadState& pad = g_pads[port];
    if (right_stick) {
        return vertical ? pad.ry : pad.rx;
    }
    return vertical ? pad.ly : pad.lx;
}

float axis_x(uint32_t port, bool right_stick)
{
    return apply_deadzone(raw_axis(port, right_stick, false), g_deadzone);
}

float axis_y(uint32_t port, bool right_stick)
{
    // The pad's vertical byte grows DOWNWARD; Unity's Y axis is up.
    return -apply_deadzone(raw_axis(port, right_stick, true), g_deadzone);
}

void set_deadzone(float value) { g_deadzone = clampf(value, 0.0f, 0.9f); }
float deadzone() { return g_deadzone; }

void set_rumble(uint32_t port, bool small_motor, uint8_t large_motor)
{
    if (port >= kMaxPorts || !g_pads[port].rumble) {
        return;
    }
#if defined(PS2UR_PLATFORM_PS2)
    char act[6] = {0, 0, 0, 0, 0, 0};
    act[0] = small_motor ? 1 : 0;
    act[1] = static_cast<char>(large_motor);
    padSetActDirect(static_cast<int>(port), 0, act);
#else
    (void)small_motor;
    (void)large_motor;
#endif
}

void inject_frame(uint32_t port, uint16_t buttons_active_low, uint8_t lx,
                  uint8_t ly, uint8_t rx, uint8_t ry, const uint8_t* pressures)
{
    if (port >= kMaxPorts) {
        return;
    }
    PadState& pad = g_pads[port];
    pad.previous = pad.buttons;
    pad.connected = true;
    pad.analog = true;
    pad.buttons = static_cast<uint16_t>(0xFFFFu ^ buttons_active_low);
    pad.lx = lx;
    pad.ly = ly;
    pad.rx = rx;
    pad.ry = ry;
    if (pressures != nullptr) {
        pad.pressure = true;
        for (uint32_t i = 0; i < 16u; ++i) {
            pad.pressures[i] = pressures[i];
        }
    }
}

} // namespace input
} // namespace ps2ur
