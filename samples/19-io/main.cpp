// samples/19-io -- M10 tasks 2, 3 and 4 on the emulated console: pad
// bring-up, a memory-card save/load round trip, and the prioritised
// streaming queue reading real files.
//
// What this can and cannot prove without hardware:
//   - Pads: the IOP modules load, both ports open, and a connected pad
//     reaches PAD_STATE_STABLE and polls cleanly. It CANNOT prove button,
//     analog, pressure or rumble behaviour -- nothing presses the buttons.
//     Those are the hardware-gated items; the mapping maths they feed is
//     covered by runtime/tests/test_input.cpp through inject_frame.
//   - Memory card: PCSX2 emulates real cards in both slots, so the save,
//     the icon.sys, the round trip and the delete are all genuinely
//     exercised. Only card-absent/unformatted/full paths need real cards.
//   - Streaming: the queue, priorities, progress and the first-access trace
//     run against real files. Reading over host: is not CDVD, so the ISO
//     seek TIMING is the part that still needs a disc.
#include <ps2ur/audio.h>
#include <ps2ur/input.h>
#include <ps2ur/log.h>
#include <ps2ur/memcard.h>
#include <ps2ur/platform.h>
#include <ps2ur/stream.h>

#include <kernel.h>
#include <stdio.h>

using namespace ps2ur;

namespace {

// Large enough for the whole music track: the queue REFUSES a file bigger
// than its destination rather than overrunning it, so an undersized buffer
// shows up as a failed request, not as corruption.
alignas(16) uint8_t g_buffer_a[1024 * 1024];
alignas(16) uint8_t g_buffer_b[64 * 1024];

const char* kSaveDir = "BASLUS-00000UNITYPS2";

uint32_t now_ms()
{
    return static_cast<uint32_t>(platform::now_ticks() /
                                 (platform::ticks_per_second() / 1000u));
}

struct Completion {
    uint32_t finished = 0;
    uint32_t first_id = 0;
};

void on_complete(void* user, uint32_t request, bool ok, uint32_t bytes)
{
    Completion* c = static_cast<Completion*>(user);
    if (c->first_id == 0) {
        c->first_id = request;
    }
    if (ok) {
        ++c->finished;
    }
    (void)bytes;
}

} // namespace

int main(void)
{
    platform::init();

    // --- Task 2: pads --------------------------------------------------------
    if (!input::init()) {
        printf("PS2UR_TOKEN_IO_FAIL input init\n");
        SleepThread();
        return 1;
    }
    {
        // Give the pads time to negotiate; a DualShock takes a while to
        // settle after the port opens.
        const uint32_t start = now_ms();
        while (now_ms() - start < 1500u) {
            input::update();
        }
        printf("M10_PADS port0 connected=%d analog=%d pressure=%d rumble=%d\n",
               input::connected(0) ? 1 : 0, input::analog_available(0) ? 1 : 0,
               input::pressure_available(0) ? 1 : 0,
               input::rumble_available(0) ? 1 : 0);
        printf("M10_PADS port1 connected=%d\n", input::connected(1) ? 1 : 0);
        printf("M10_PADS sticks l=(%u,%u) r=(%u,%u) buttons_held=%d\n",
               static_cast<unsigned>(input::raw_axis(0, false, false)),
               static_cast<unsigned>(input::raw_axis(0, false, true)),
               static_cast<unsigned>(input::raw_axis(0, true, false)),
               static_cast<unsigned>(input::raw_axis(0, true, true)),
               input::button(0, input::Button::Cross) ? 1 : 0);

        // The polling path has to survive a pad that is absent as cleanly as
        // one that is present -- an unplugged port must not wedge the loop.
        for (uint32_t i = 0; i < 60u; ++i) {
            input::update();
        }
        // Injected frames prove the mapping the emulator cannot press.
        input::inject_frame(0, static_cast<uint16_t>(~(1u << 14)), 255, 0, 128,
                            128, nullptr);
        const bool cross = input::button(0, input::Button::Cross);
        const float ax = input::axis_x(0, false);
        const float ay = input::axis_y(0, false);
        printf("M10_PADS injected cross=%d axis=(%.2f,%.2f)\n", cross ? 1 : 0,
               static_cast<double>(ax), static_cast<double>(ay));
        if (!cross || ax < 0.9f || ay < 0.9f) {
            printf("PS2UR_TOKEN_IO_FAIL injected pad frame\n");
            SleepThread();
            return 1;
        }
    }

    // --- Task 3: memory card -------------------------------------------------
    if (!memcard::init()) {
        printf("PS2UR_TOKEN_IO_FAIL memcard init\n");
        SleepThread();
        return 1;
    }
    {
        memcard::CardInfo info;
        const memcard::Status probe = memcard::probe(0, &info);
        printf("M10_CARD slot0 status='%s' present=%d formatted=%d free=%d\n",
               memcard::status_text(probe), info.present ? 1 : 0,
               info.formatted ? 1 : 0, static_cast<int>(info.free_clusters));

        if (probe == memcard::Status::NotFormatted) {
            // A card the browser has never formatted is a real state, and
            // the plan requires handling it. A shipping game asks the player
            // first: formatting erases every save on the card.
            printf("M10_CARD unformatted; formatting (test card only)\n");
            const memcard::Status formatted = memcard::format(0);
            printf("M10_CARD format='%s'\n", memcard::status_text(formatted));
            if (formatted == memcard::Status::Ok) {
                memcard::probe(0, &info);
            }
        }
        if (!info.present || !info.formatted) {
            printf("PS2UR_TOKEN_IO_FAIL no usable card in slot 0\n");
            SleepThread();
            return 1;
        }

        // Start clean so a rerun measures a real write, not a leftover.
        memcard::delete_save(0, kSaveDir);

        memcard::prefs::clear();
        memcard::prefs::set_int("level", 7);
        memcard::prefs::set_float("volume", 0.75f);
        memcard::prefs::set_string("player", "ASH");
        const memcard::Status saved =
            memcard::prefs::save(0, kSaveDir, "Unity PS2 Save");
        printf("M10_CARD save='%s'\n", memcard::status_text(saved));
        if (saved != memcard::Status::Ok) {
            printf("PS2UR_TOKEN_IO_FAIL save\n");
            SleepThread();
            return 1;
        }

        // Wipe the in-memory store so a successful read is the only way the
        // values can come back.
        memcard::prefs::clear();
        if (memcard::prefs::get_int("level", -1) != -1) {
            printf("PS2UR_TOKEN_IO_FAIL prefs not cleared\n");
            SleepThread();
            return 1;
        }

        const memcard::Status loaded = memcard::prefs::load(0, kSaveDir);
        printf("M10_CARD load='%s' level=%d volume=%.2f player='%s'\n",
               memcard::status_text(loaded),
               static_cast<int>(memcard::prefs::get_int("level", -1)),
               static_cast<double>(memcard::prefs::get_float("volume", -1.0f)),
               memcard::prefs::get_string("player", "?"));
        if (loaded != memcard::Status::Ok ||
            memcard::prefs::get_int("level", -1) != 7 ||
            memcard::prefs::get_float("volume", -1.0f) < 0.74f ||
            memcard::prefs::get_float("volume", -1.0f) > 0.76f) {
            printf("PS2UR_TOKEN_IO_FAIL round trip\n");
            SleepThread();
            return 1;
        }

        // icon.sys must exist, or the console browser shows an unnamed save.
        if (!memcard::save_exists(0, kSaveDir, "icon.sys")) {
            printf("PS2UR_TOKEN_IO_FAIL icon.sys missing\n");
            SleepThread();
            return 1;
        }
        printf("M10_CARD icon.sys present\n");
    }

    // --- Task 4: streaming ---------------------------------------------------
    if (!stream::init()) {
        printf("PS2UR_TOKEN_IO_FAIL stream init\n");
        SleepThread();
        return 1;
    }
    {
        stream::trace_reset();
        Completion completion;

        // A big low-priority read submitted FIRST, then a small high-priority
        // one behind it: the audio refill must not queue behind the level.
        const uint32_t low =
            stream::request("host:music.raw", g_buffer_a, sizeof(g_buffer_a),
                            stream::Priority::Low, on_complete, &completion);
        const uint32_t high =
            stream::request("host:audio.p2b", g_buffer_b, sizeof(g_buffer_b),
                            stream::Priority::High, on_complete, &completion);
        if (low == 0 || high == 0) {
            printf("PS2UR_TOKEN_IO_FAIL stream queue\n");
            SleepThread();
            return 1;
        }

        uint32_t frames = 0;
        const uint32_t start = now_ms();
        while (stream::pending() > 0 && frames < 4000u) {
            stream::update(32 * 1024); // one chunk per "frame"
            ++frames;
        }
        const uint32_t elapsed = now_ms() - start;
        printf("M10_STREAM frames=%u elapsed=%u ms progress=%.2f first=%u "
               "high=%u finished=%u\n",
               static_cast<unsigned>(frames), static_cast<unsigned>(elapsed),
               static_cast<double>(stream::progress()),
               static_cast<unsigned>(completion.first_id),
               static_cast<unsigned>(high),
               static_cast<unsigned>(completion.finished));

        if (completion.first_id != high) {
            printf("PS2UR_TOKEN_IO_FAIL priority order\n");
            SleepThread();
            return 1;
        }
        if (stream::state(low) != stream::RequestState::Done ||
            stream::state(high) != stream::RequestState::Done) {
            printf("PS2UR_TOKEN_IO_FAIL stream did not complete\n");
            SleepThread();
            return 1;
        }
        if (frames < 4u) {
            // A whole-file gulp would defeat the point of the queue.
            printf("PS2UR_TOKEN_IO_FAIL queue did not chunk the reads\n");
            SleepThread();
            return 1;
        }

        // The first-access trace is what orders files on the disc (M10 task
        // 4). It records first touch, in order, ignoring repeats.
        printf("M10_TRACE %u entries:\n",
               static_cast<unsigned>(stream::trace_count()));
        for (uint32_t i = 0; i < stream::trace_count(); ++i) {
            printf("M10_TRACE   %u %s\n", static_cast<unsigned>(i),
                   stream::trace_entry(i));
        }
        if (stream::trace_count() != 2u) {
            printf("PS2UR_TOKEN_IO_FAIL trace\n");
            SleepThread();
            return 1;
        }
    }

    printf("PS2UR_TOKEN_IO_OK\n");
    stream::shutdown();
    memcard::shutdown();
    input::shutdown();
    SleepThread();
    return 0;
}
