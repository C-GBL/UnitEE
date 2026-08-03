// audio: SPU2 sample playback with voice allocation and priority stealing,
// per-source volume/pan, simple 3D attenuation, and streamed music
// (plan section 9, M10 task 1).
//
// Built on audsrv.irx, which the plan names as the starting point. What that
// buys and what it costs:
//
//   + 24 SPU2 voices with ADPCM upload, per-channel volume and pan, and a
//     "is this channel still playing" query -- enough to implement voice
//     allocation and priority stealing on the EE side, which is where the
//     policy belongs anyway.
//   - Pitch is a property of the LOADED SAMPLE (audsrv_adpcm_t::pitch), not
//     of a playback. Per-source pitch variation therefore needs either the
//     same clip loaded at several pitches or the custom .irx the plan holds
//     in reserve. Recorded as a deviation in docs/supported-api.md rather
//     than faked with a member that silently does nothing.
//
// Clips are 16-byte-header "APCM" blobs produced by the exporter and carried
// in the .p2b SND section (docs/formats/p2b-container.md).
#pragma once

#include "ps2ur/math.h"

#include <cstdint>

namespace ps2ur {
namespace audio {

// SPU2 has 24 voices; audsrv exposes all of them.
inline constexpr uint32_t kMaxVoices = 24;
inline constexpr uint32_t kMaxClips = 32;

// A handle to a playing voice. Zero is never valid, so a default-constructed
// handle is silent rather than pointing at voice 0.
typedef int32_t VoiceHandle;

struct ClipInfo {
    uint32_t name_hash = 0;
    uint32_t pitch = 0; // SPU2 pitch: freq * 4096 / 48000
    uint32_t sample_count = 0;
    uint32_t channels = 1;
};

// Volume is 0..1; pan is -1 (left) .. +1 (right).
struct PlayParams {
    float volume = 1.0f;
    float pan = 0.0f;
    // Higher wins when every voice is busy. A sound may only steal a voice
    // playing something of STRICTLY lower priority, so a looping music stem
    // at priority 200 is never cut short by footsteps at 10.
    int32_t priority = 0;
};

// 3D attenuation, Unity's linear rolloff between min and max distance.
struct Attenuation3D {
    Vec3 position{0, 0, 0};
    float min_distance = 1.0f;
    float max_distance = 30.0f;
};

bool init();
void shutdown();
bool initialized();

// Uploads a clip to SPU2 memory. 'data' is the "APCM" blob and must stay
// valid until shutdown. Returns the clip index, or -1.
int32_t load_clip(const void* data, uint32_t size);
uint32_t clip_count();
const ClipInfo& clip_info(uint32_t index);

// Stops every voice and forgets every clip, reclaiming the SPU2 memory in
// one move (audsrv rebuilds its ADPCM arena). For a Single scene load,
// where the incoming scene brings its own SND section and the outgoing
// scene's blobs are about to be overwritten in the arena they live in.
void reset_clips();

// Starts a clip. Returns a voice handle, or 0 when every voice is busy with
// something of equal or higher priority (a dropped sound is normal and must
// never be an error).
VoiceHandle play(uint32_t clip_index, const PlayParams& params);

// Starts a clip positioned in the world; volume and pan are derived from
// the listener set by set_listener().
VoiceHandle play_3d(uint32_t clip_index, const PlayParams& params,
                    const Attenuation3D& attenuation);

void stop(VoiceHandle voice);
void stop_all();
bool is_playing(VoiceHandle voice);

// Live changes to a sounding voice.
void set_voice_volume(VoiceHandle voice, float volume, float pan);

// The AudioListener. 'right' is the listener's right vector, used for pan.
void set_listener(Vec3 position, Vec3 right);

// Volume and pan a 3D source would get right now: the attenuation curve on
// its own, so it can be tested without an SPU2.
void evaluate_3d(const Attenuation3D& attenuation, float source_volume,
                 float* out_volume, float* out_pan);

// True when the mixer can cut a sounding voice short for a more important
// one. FALSE on audsrv, which offers no way to stop an ADPCM channel: a
// sound arriving when all 24 voices are busy is dropped rather than
// stealing (M10, verify-log). The policy below still says WHICH voice would
// be sacrificed, so it is testable now and the custom .irx has a target.
bool can_preempt_voices();

// The voice a sound of this priority would take if preemption were
// available, or -1 when nothing sounding is less important.
int32_t steal_candidate_voice(int32_t priority);

// Voices currently sounding, for the debug overlay and the tests.
uint32_t active_voice_count();
// Sounds dropped because every voice was busy with higher-priority audio.
uint32_t dropped_count();

// ---- streamed music (M10 task 1, "double-buffered ... with underrun
// detection") ---------------------------------------------------------------
//
// The feeder hands audsrv PCM in fixed chunks. Its source is a callback so
// the same machinery serves a file on disc, a decoder, or a test generator.
// Returns the number of bytes written; a short read ends the stream.
typedef uint32_t (*MusicReadFn)(void* user, void* dest, uint32_t bytes);

bool music_start(MusicReadFn read, void* user, uint32_t freq, uint32_t bits,
                 uint32_t channels);
void music_stop();
bool music_playing();
// Call once per frame. Refills whatever the SPU2 has consumed; returns false
// when the source ran dry.
bool music_update();
// Times the feeder arrived to find the buffer already empty: the audible
// symptom is a gap, so this is the number the acceptance asserts.
uint32_t music_underruns();
uint32_t music_bytes_fed();

} // namespace audio
} // namespace ps2ur
