#include "ps2ur/audio.h"

#include "ps2ur/log.h"

#if defined(PS2UR_PLATFORM_PS2)
#include <audsrv.h>
#include <kernel.h>
#include <loadfile.h>
#include <sifrpc.h>

extern "C" {
extern unsigned char audsrv_irx_start[];
extern unsigned char audsrv_irx_end[];
}
#endif

namespace ps2ur {
namespace audio {

namespace {

bool g_initialized = false;

struct Clip {
    ClipInfo info;
#if defined(PS2UR_PLATFORM_PS2)
    audsrv_adpcm_t adpcm;
#endif
    bool loaded = false;
};

struct Voice {
    bool active = false;
    int32_t priority = 0;
    uint32_t clip = 0;
    uint32_t generation = 1; // retires stale handles, like entity handles
};

Clip g_clips[kMaxClips];
uint32_t g_clip_count = 0;
Voice g_voices[kMaxVoices];
uint32_t g_dropped = 0;

Vec3 g_listener_pos{0, 0, 0};
Vec3 g_listener_right{1, 0, 0};

// Music streaming state.
constexpr uint32_t kMusicChunk = 8 * 1024;
// Below this a top-up is not worth a read; audsrv's ring is small.
constexpr uint32_t kMinFeed = 512;
uint32_t g_music_ring_max = 0;
MusicReadFn g_music_read = nullptr;
void* g_music_user = nullptr;
bool g_music_playing = false;
uint32_t g_music_underruns = 0;
uint32_t g_music_bytes = 0;
alignas(64) uint8_t g_music_buffer[kMusicChunk];

float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

#if defined(PS2UR_PLATFORM_PS2)
// audsrv volume is 0..MAX_VOLUME (0x3FFF); pan is -100..100.
int32_t to_audsrv_volume(float volume)
{
    return static_cast<int32_t>(clampf(volume, 0.0f, 1.0f) * 0x3FFF);
}

int32_t to_audsrv_pan(float pan)
{
    return static_cast<int32_t>(clampf(pan, -1.0f, 1.0f) * 100.0f);
}
#endif

VoiceHandle make_handle(uint32_t index)
{
    return static_cast<VoiceHandle>((g_voices[index].generation << 8) |
                                    (index + 1u));
}

int32_t resolve_voice(VoiceHandle handle)
{
    const uint32_t h = static_cast<uint32_t>(handle);
    const int32_t index = static_cast<int32_t>(h & 0xFFu) - 1;
    if (index < 0 || index >= static_cast<int32_t>(kMaxVoices)) {
        return -1;
    }
    if (!g_voices[index].active || (h >> 8) != g_voices[index].generation) {
        return -1;
    }
    return index;
}

// Refreshes the active flags from the hardware: a one-shot ends on its own,
// and a finished voice is free to reuse without stealing anything.
void refresh_voices()
{
#if defined(PS2UR_PLATFORM_PS2)
    for (uint32_t i = 0; i < kMaxVoices; ++i) {
        if (!g_voices[i].active) {
            continue;
        }
        Clip& clip = g_clips[g_voices[i].clip];
        if (!audsrv_is_adpcm_playing(static_cast<int>(i), &clip.adpcm)) {
            g_voices[i].active = false;
        }
    }
#endif
}

// Whether a sounding voice can be cut short to make room for a more
// important one.
//
// MEASURED FALSE on audsrv (M10, verify-log): its ADPCM interface exposes
// play, set-volume-and-pan and is-playing, but NO way to stop a channel.
// audsrv_ch_play_adpcm on an occupied channel returns
// -AUDSRV_ERR_NO_MORE_CHANNELS, so a busy voice cannot be preempted at all.
// The plan anticipates exactly this ("write a custom .irx only if the mixing
// model proves insufficient"); this constant is the evidence and the switch
// that flips when that IRX exists.
//
// The host build honours the same constraint deliberately. A host build that
// allowed preemption would let tests assert behaviour the console cannot
// deliver, which is worse than not testing it.
inline constexpr bool kCanPreemptVoices = false;

// Picks a voice for a sound of the given priority: a free one first, then --
// where the mixer allows it -- the lowest-priority sounding voice, and only
// when that priority is strictly lower (M10 task 1).
int32_t allocate_voice(int32_t priority)
{
    for (uint32_t i = 0; i < kMaxVoices; ++i) {
        if (!g_voices[i].active) {
            return static_cast<int32_t>(i);
        }
    }
    if (!kCanPreemptVoices) {
        return -1;
    }
    int32_t victim = -1;
    int32_t lowest = priority;
    for (uint32_t i = 0; i < kMaxVoices; ++i) {
        if (g_voices[i].priority < lowest) {
            lowest = g_voices[i].priority;
            victim = static_cast<int32_t>(i);
        }
    }
    return victim;
}

// Which voice WOULD be sacrificed for a sound of this priority. Exposed so
// the policy stays testable while the mixer cannot act on it, and so the
// custom IRX has a specification to satisfy.
int32_t steal_candidate(int32_t priority)
{
    int32_t victim = -1;
    int32_t lowest = priority;
    for (uint32_t i = 0; i < kMaxVoices; ++i) {
        if (g_voices[i].active && g_voices[i].priority < lowest) {
            lowest = g_voices[i].priority;
            victim = static_cast<int32_t>(i);
        }
    }
    return victim;
}

uint32_t rd_u32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

bool init()
{
    if (g_initialized) {
        return true;
    }
#if defined(PS2UR_PLATFORM_PS2)
    // audsrv needs the SPU2 driver, which is in ROM on every console.
    int ret = SifLoadModule("rom0:LIBSD", 0, nullptr);
    if (ret < 0) {
        log(LogLevel::Error, "audio: rom0:LIBSD failed (%d)", ret);
        return false;
    }
    // audsrv is loaded FROM A FILE, not from the embedded blob.
    //
    // SifExecModuleBuffer works for iomanX and fileXio but leaves audsrv
    // un-started: it returns a plausible-looking id, the module's _start
    // never runs (no greeting, no RegisterLibraryEntries), and audsrv_init
    // then waits forever on an RPC server that does not exist. A file load
    // of the identical .irx works, which is also how shipping titles do it
    // -- IRX modules live on the disc. The blob stays embedded as a last
    // resort for a filesystem-less boot. See verify-log, M10.
    int module_result = 0;
    static const char* const kAudsrvPaths[] = {
        "host:audsrv.irx",
        "cdrom0:\\AUDSRV.IRX;1",
        "mass:audsrv.irx",
    };
    ret = -1;
    for (uint32_t i = 0; i < 3u && ret < 0; ++i) {
        ret = SifLoadStartModule(kAudsrvPaths[i], 0, nullptr, &module_result);
        if (ret >= 0) {
            log(LogLevel::Debug, "audio: audsrv from '%s' (id %d)",
                kAudsrvPaths[i], ret);
        }
    }
    if (ret < 0) {
        FlushCache(0);
        ret = SifExecModuleBuffer(
            audsrv_irx_start,
            static_cast<unsigned>(audsrv_irx_end - audsrv_irx_start), 0,
            nullptr, &module_result);
        log(LogLevel::Warn, "audio: audsrv file load failed; embedded blob "
                            "returned %d", ret);
    }
    if (ret < 0) {
        log(LogLevel::Error, "audio: audsrv.irx failed (%d)", ret);
        return false;
    }
    ret = audsrv_init();
    if (ret != 0) {
        log(LogLevel::Error, "audio: audsrv_init failed: %s",
            audsrv_get_error_string());
        return false;
    }
    audsrv_adpcm_init();
    audsrv_set_volume(0x3FFF);
#endif
    for (uint32_t i = 0; i < kMaxVoices; ++i) {
        g_voices[i] = Voice{};
    }
    g_clip_count = 0;
    g_dropped = 0;
    g_initialized = true;
    log(LogLevel::Debug, "audio: init, %u voices",
        static_cast<unsigned>(kMaxVoices));
    return true;
}

void shutdown()
{
    if (!g_initialized) {
        return;
    }
    stop_all();
    music_stop();
#if defined(PS2UR_PLATFORM_PS2)
    audsrv_quit();
#endif
    g_initialized = false;
}

bool initialized() { return g_initialized; }

int32_t load_clip(const void* data, uint32_t size)
{
    if (!g_initialized || data == nullptr || g_clip_count >= kMaxClips) {
        return -1;
    }
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    // "APCM" header (docs/formats/p2b-container.md): magic, version,
    // channels, pitch, sample count, then 16-byte ADPCM blocks.
    if (size < 16u || bytes[0] != 'A' || bytes[1] != 'P' || bytes[2] != 'C' ||
        bytes[3] != 'M') {
        log(LogLevel::Error, "audio: clip is not an APCM blob");
        return -1;
    }
    Clip& clip = g_clips[g_clip_count];
    clip.info.channels = bytes[5] != 0 ? bytes[5] : 1u;
    clip.info.pitch = rd_u32(bytes + 8);
    clip.info.sample_count = rd_u32(bytes + 12);
    clip.info.name_hash = 0;

#if defined(PS2UR_PLATFORM_PS2)
    // audsrv wants the header too: it reads pitch and channels from it.
    if (audsrv_load_adpcm(&clip.adpcm, const_cast<void*>(data),
                          static_cast<int>(size)) != 0) {
        log(LogLevel::Error, "audio: audsrv_load_adpcm failed: %s",
            audsrv_get_error_string());
        return -1;
    }
#endif
    clip.loaded = true;
    return static_cast<int32_t>(g_clip_count++);
}

uint32_t clip_count() { return g_clip_count; }

const ClipInfo& clip_info(uint32_t index)
{
    static ClipInfo empty;
    return index < g_clip_count ? g_clips[index].info : empty;
}

VoiceHandle play(uint32_t clip_index, const PlayParams& params)
{
    if (!g_initialized || clip_index >= g_clip_count) {
        return 0;
    }
    refresh_voices();
    const int32_t voice = allocate_voice(params.priority);
    if (voice < 0) {
        // Every voice is busy with something at least as important. Dropping
        // the sound is the correct outcome, not an error.
        ++g_dropped;
        return 0;
    }

#if defined(PS2UR_PLATFORM_PS2)
    Clip& clip = g_clips[clip_index];
    const int played = audsrv_ch_play_adpcm(voice, &clip.adpcm);
    if (played < 0) {
        ++g_dropped;
        return 0;
    }
    audsrv_adpcm_set_volume_and_pan(voice, to_audsrv_volume(params.volume),
                                    to_audsrv_pan(params.pan));
#endif

    // A stolen voice retires its old handle.
    g_voices[voice].generation =
        static_cast<uint32_t>((g_voices[voice].generation + 1u) & 0xFFFFFFu);
    if (g_voices[voice].generation == 0) {
        g_voices[voice].generation = 1;
    }
    g_voices[voice].active = true;
    g_voices[voice].priority = params.priority;
    g_voices[voice].clip = clip_index;
    return make_handle(static_cast<uint32_t>(voice));
}

void evaluate_3d(const Attenuation3D& attenuation, float source_volume,
                 float* out_volume, float* out_pan)
{
    const Vec3 to_source = sub(attenuation.position, g_listener_pos);
    const float distance = length(to_source);

    // Unity's linear rolloff: full volume inside min_distance, silent past
    // max_distance, linear between.
    float attenuated;
    if (distance <= attenuation.min_distance) {
        attenuated = 1.0f;
    } else if (distance >= attenuation.max_distance) {
        attenuated = 0.0f;
    } else {
        const float span = attenuation.max_distance - attenuation.min_distance;
        attenuated = 1.0f - (distance - attenuation.min_distance) / span;
    }

    // Pan by how far the source sits along the listener's right vector.
    float pan = 0.0f;
    if (distance > 1e-4f) {
        pan = dot(scale(to_source, 1.0f / distance), g_listener_right);
    }
    if (out_volume != nullptr) {
        *out_volume = clampf(source_volume * attenuated, 0.0f, 1.0f);
    }
    if (out_pan != nullptr) {
        *out_pan = clampf(pan, -1.0f, 1.0f);
    }
}

VoiceHandle play_3d(uint32_t clip_index, const PlayParams& params,
                    const Attenuation3D& attenuation)
{
    PlayParams positioned = params;
    evaluate_3d(attenuation, params.volume, &positioned.volume, &positioned.pan);
    if (positioned.volume <= 0.0f) {
        return 0; // out of range: never spend a voice on silence
    }
    return play(clip_index, positioned);
}

// Releases the caller's claim on a voice.
//
// On a mixer that cannot preempt (audsrv), this CANNOT silence the sample:
// the sound plays to its end and the voice stays busy until the hardware
// says otherwise. Retiring the handle without freeing the slot is the only
// honest model -- marking the voice free here would hand it to the next
// caller, whose play would then be refused by a channel that is still
// sounding, and the sound would vanish for no visible reason.
void stop(VoiceHandle handle)
{
    const int32_t voice = resolve_voice(handle);
    if (voice < 0) {
        return;
    }
    // Retire the handle either way.
    g_voices[voice].generation =
        static_cast<uint32_t>((g_voices[voice].generation + 1u) & 0xFFFFFFu);
    if (g_voices[voice].generation == 0) {
        g_voices[voice].generation = 1;
    }
#if defined(PS2UR_PLATFORM_PS2)
    // Leave it active; refresh_voices() frees it when the sample ends.
    g_voices[voice].priority = 0; // it is nobody's sound now
#else
    // The host build has no SPU2 and therefore no notion of a sample
    // ending, so it models an instantly-draining mixer: stop frees the
    // slot. The rule that MATTERS -- a sounding voice cannot be preempted
    // -- is shared with the console, which is what the tests exercise.
    g_voices[voice].active = false;
#endif
}

void stop_all()
{
    for (uint32_t i = 0; i < kMaxVoices; ++i) {
        if (!g_voices[i].active) {
            continue;
        }
        g_voices[i].generation =
            static_cast<uint32_t>((g_voices[i].generation + 1u) & 0xFFFFFFu);
        if (g_voices[i].generation == 0) {
            g_voices[i].generation = 1;
        }
#if defined(PS2UR_PLATFORM_PS2)
        g_voices[i].priority = 0;
#else
        g_voices[i].active = false;
#endif
    }
}

bool is_playing(VoiceHandle handle)
{
    refresh_voices();
    return resolve_voice(handle) >= 0;
}

void set_voice_volume(VoiceHandle handle, float volume, float pan)
{
    const int32_t voice = resolve_voice(handle);
    if (voice < 0) {
        return;
    }
#if defined(PS2UR_PLATFORM_PS2)
    audsrv_adpcm_set_volume_and_pan(voice, to_audsrv_volume(volume),
                                    to_audsrv_pan(pan));
#else
    (void)volume;
    (void)pan;
#endif
}

void set_listener(Vec3 position, Vec3 right)
{
    g_listener_pos = position;
    const float len = length(right);
    g_listener_right = len > 1e-6f ? scale(right, 1.0f / len) : Vec3{1, 0, 0};
}

uint32_t active_voice_count()
{
    refresh_voices();
    uint32_t count = 0;
    for (uint32_t i = 0; i < kMaxVoices; ++i) {
        if (g_voices[i].active) {
            ++count;
        }
    }
    return count;
}

uint32_t dropped_count() { return g_dropped; }

bool can_preempt_voices() { return kCanPreemptVoices; }

int32_t steal_candidate_voice(int32_t priority)
{
    refresh_voices();
    return steal_candidate(priority);
}

// ---- streamed music -------------------------------------------------------

bool music_start(MusicReadFn read, void* user, uint32_t freq, uint32_t bits,
                 uint32_t channels)
{
    if (!g_initialized || read == nullptr) {
        return false;
    }
#if defined(PS2UR_PLATFORM_PS2)
    audsrv_fmt_t format;
    format.freq = static_cast<int>(freq);
    format.bits = static_cast<int>(bits);
    format.channels = static_cast<int>(channels);
    if (audsrv_set_format(&format) != 0) {
        log(LogLevel::Error, "audio: audsrv_set_format failed: %s",
            audsrv_get_error_string());
        return false;
    }
#else
    (void)freq;
    (void)bits;
    (void)channels;
#endif
    g_music_read = read;
    g_music_user = user;
    g_music_playing = true;
    g_music_underruns = 0;
    g_music_bytes = 0;
    g_music_ring_max = 0;
    // Prime both halves before the SPU2 starts consuming, so the first frame
    // is never a race between the feeder and the hardware. A stream shorter
    // than two chunks still starts: what matters is that the FIRST read
    // produced audio, not that the source could fill the whole ring.
    // Prime as much as the ring will take before playback starts, so the
    // first frames are never a race between the feeder and the hardware.
    for (uint32_t i = 0; i < 16u && g_music_playing; ++i) {
        music_update();
    }
    // Priming filled the ring, so the first real update must not read that
    // as a drained buffer.
    g_music_underruns = 0;
    if (g_music_bytes == 0) {
        music_stop();
        return false;
    }
    return true;
}

void music_stop()
{
    if (!g_music_playing) {
        return;
    }
#if defined(PS2UR_PLATFORM_PS2)
    audsrv_stop_audio();
#endif
    g_music_playing = false;
    g_music_read = nullptr;
}

bool music_playing() { return g_music_playing; }

bool music_update()
{
    if (!g_music_playing || g_music_read == nullptr) {
        return false;
    }
    uint32_t want = kMusicChunk;
#if defined(PS2UR_PLATFORM_PS2)
    // NON-BLOCKING by design. audsrv_wait_audio() sleeps until the ring has
    // room, which is exactly what a frame loop must never do: a blocking
    // feeder would stall the game for whole frames every time it topped up.
    // Instead ask how much room there is and feed that much.
    //
    // The ring's size is audsrv's business, not ours -- it is smaller than a
    // comfortable chunk -- so the feed adapts rather than assuming.
    const int room = audsrv_available();
    if (room > static_cast<int>(g_music_ring_max)) {
        g_music_ring_max = static_cast<uint32_t>(room);
    }
    if (room < static_cast<int>(kMinFeed)) {
        return true; // still playing; nothing to do this frame
    }
    // A completely empty ring after we have started feeding means the SPU2
    // drained everything before we came back: the gap a listener hears.
    if (g_music_bytes > 0 && g_music_ring_max > 0 &&
        static_cast<uint32_t>(room) >= g_music_ring_max) {
        ++g_music_underruns;
    }
    if (want > static_cast<uint32_t>(room)) {
        want = static_cast<uint32_t>(room);
    }
    want &= ~3u; // whole stereo 16-bit frames
    if (want == 0) {
        return true;
    }
#endif
    const uint32_t got = g_music_read(g_music_user, g_music_buffer, want);
    if (got == 0) {
        music_stop();
        return false;
    }
#if defined(PS2UR_PLATFORM_PS2)
    audsrv_play_audio(reinterpret_cast<const char*>(g_music_buffer),
                      static_cast<int>(got));
#endif
    g_music_bytes += got;
    return true;
}

uint32_t music_underruns() { return g_music_underruns; }
uint32_t music_bytes_fed() { return g_music_bytes; }

} // namespace audio
} // namespace ps2ur
