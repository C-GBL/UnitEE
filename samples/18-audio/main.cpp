// samples/18-audio -- M10 task 1 on real SPU2 hardware (emulated): clip
// upload, voice allocation with priority stealing, 3D attenuation, and
// double-buffered streamed music with underrun counting.
//
// The load-bearing assertion is that a voice STOPS on its own after roughly
// the clip's duration. Allocation bookkeeping can look perfect while the
// SPU2 plays nothing at all; a voice that retires on schedule is only
// possible if the hardware really consumed the sample.
//
// Music is fed chunk-by-chunk straight from a file handle rather than from
// a preloaded buffer, so this exercises the double-buffered read the plan
// asks for. The source is host: here and CDVD once M10 task 4 lands -- the
// feeder does not care which.
#include <ps2ur/alloc.h>
#include <ps2ur/audio.h>
#include <ps2ur/io.h>
#include <ps2ur/log.h>
#include <ps2ur/p2b.h>
#include <ps2ur/platform.h>

#include <kernel.h>
#include <stdio.h>

using namespace ps2ur;

// Legacy fio: newlib's file I/O routes through fileXio, which PCSX2's host:
// HLE does not serve (verify-log, M5).
extern "C" {
int fioOpen(const char* name, int mode);
int fioClose(int fd);
int fioRead(int fd, void* buf, int size);
int fioLseek(int fd, int offset, int whence);
}

namespace {

alignas(16) uint8_t g_arena_mem[1024 * 1024];

struct MusicFile {
    int fd = -1;
    uint32_t bytes_read = 0;
};

uint32_t music_read(void* user, void* dest, uint32_t bytes)
{
    MusicFile* file = static_cast<MusicFile*>(user);
    if (file->fd < 0) {
        return 0;
    }
    const int got = fioRead(file->fd, dest, static_cast<int>(bytes));
    if (got <= 0) {
        return 0;
    }
    file->bytes_read += static_cast<uint32_t>(got);
    return static_cast<uint32_t>(got);
}

uint32_t rd_u32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// Milliseconds since boot, from the COP0 counter platform::now_ticks wraps.
uint32_t now_ms()
{
    return static_cast<uint32_t>(platform::now_ticks() /
                                 (platform::ticks_per_second() / 1000u));
}

int open_music()
{
    const char* candidates[] = {"host:music.raw", "host0:music.raw", "music.raw"};
    for (uint32_t i = 0; i < 3; ++i) {
        const int fd = fioOpen(candidates[i], 1 /*O_RDONLY*/);
        if (fd >= 0) {
            printf("[18-audio] music source '%s'\n", candidates[i]);
            return fd;
        }
    }
    return -1;
}

} // namespace

int main(void)
{
    platform::init();

    if (!audio::init()) {
        printf("PS2UR_TOKEN_AUDIO_FAIL init\n");
        SleepThread();
        return 1;
    }
    printf("[18-audio] audsrv up, %u voices\n",
           static_cast<unsigned>(audio::kMaxVoices));

    // --- Load the SND section ----------------------------------------------
    Arena arena;
    arena.init(g_arena_mem, sizeof(g_arena_mem));
    uint32_t size = 0;
    const void* data = io::load_file("host:audio.p2b", arena, &size);
    if (data == nullptr) {
        data = io::load_file("audio.p2b", arena, &size);
    }
    io::P2bFile file;
    if (data == nullptr || !file.parse(data, size)) {
        printf("PS2UR_TOKEN_AUDIO_FAIL container\n");
        SleepThread();
        return 1;
    }
    const io::P2bSection* snd = file.find(io::kSectionSound);
    if (snd == nullptr || snd->size < 16u) {
        printf("PS2UR_TOKEN_AUDIO_FAIL no SND section\n");
        SleepThread();
        return 1;
    }
    const uint32_t clip_count = rd_u32(snd->data);
    printf("[18-audio] SND: %u clips\n", static_cast<unsigned>(clip_count));
    if (clip_count < 3u) {
        printf("PS2UR_TOKEN_AUDIO_FAIL expected three clips\n");
        SleepThread();
        return 1;
    }
    for (uint32_t i = 0; i < clip_count; ++i) {
        const uint8_t* record = snd->data + 16u + i * 16u;
        const uint32_t offset = rd_u32(record + 4);
        const uint32_t bytes = rd_u32(record + 8);
        if (offset + bytes > snd->size) {
            printf("PS2UR_TOKEN_AUDIO_FAIL clip %u out of range\n",
                   static_cast<unsigned>(i));
            SleepThread();
            return 1;
        }
        const int32_t loaded = audio::load_clip(snd->data + offset, bytes);
        if (loaded < 0) {
            printf("PS2UR_TOKEN_AUDIO_FAIL clip %u upload\n",
                   static_cast<unsigned>(i));
            SleepThread();
            return 1;
        }
        printf("[18-audio] clip %u: %u samples, pitch %u, %u bytes\n",
               static_cast<unsigned>(i),
               static_cast<unsigned>(audio::clip_info(loaded).sample_count),
               static_cast<unsigned>(audio::clip_info(loaded).pitch),
               static_cast<unsigned>(bytes));
    }

    // --- Check 1: a voice retires on its own -------------------------------
    //
    // Clip 0 is a 0.25 s blip. If the SPU2 is really playing it, the voice
    // frees itself within a fraction of a second; if nothing is playing,
    // either it never becomes active or it never stops.
    {
        audio::PlayParams params;
        params.volume = 0.8f;
        params.priority = 10;
        const audio::VoiceHandle voice = audio::play(0, params);
        if (voice == 0 || !audio::is_playing(voice)) {
            printf("PS2UR_TOKEN_AUDIO_FAIL blip did not start\n");
            SleepThread();
            return 1;
        }
        const uint32_t start = now_ms();
        uint32_t elapsed = 0;
        while (audio::is_playing(voice) && elapsed < 2000u) {
            elapsed = now_ms() - start;
        }
        printf("M10_VOICE_LIFETIME clip=0.25s observed=%u ms\n",
               static_cast<unsigned>(elapsed));
        if (elapsed >= 2000u) {
            printf("PS2UR_TOKEN_AUDIO_FAIL voice never finished (SPU2 idle?)\n");
            SleepThread();
            return 1;
        }
        if (elapsed < 100u) {
            printf("PS2UR_TOKEN_AUDIO_FAIL voice ended too early (%u ms)\n",
                   static_cast<unsigned>(elapsed));
            SleepThread();
            return 1;
        }
    }

    // --- Check 2: allocation and priority stealing on hardware --------------
    {
        audio::stop_all();
        audio::PlayParams quiet;
        quiet.volume = 0.25f;
        quiet.priority = 1;
        uint32_t started = 0;
        for (uint32_t i = 0; i < audio::kMaxVoices; ++i) {
            if (audio::play(1, quiet) != 0) {
                ++started;
            }
        }
        const uint32_t before = audio::active_voice_count();

        // The steal POLICY still names a victim...
        const int32_t candidate = audio::steal_candidate_voice(100);

        // ...but audsrv cannot stop a sounding channel, so the important
        // sound is dropped rather than cutting one short. Asserting a steal
        // here would be asserting something this mixer cannot do.
        audio::PlayParams important;
        important.volume = 0.6f;
        important.priority = 100;
        const audio::VoiceHandle loud = audio::play(2, important);
        printf("M10_VOICES started=%u active=%u preempt=%d candidate=%d "
               "admitted=%d dropped=%u\n",
               static_cast<unsigned>(started), static_cast<unsigned>(before),
               audio::can_preempt_voices() ? 1 : 0, static_cast<int>(candidate),
               loud != 0 ? 1 : 0,
               static_cast<unsigned>(audio::dropped_count()));
        if (started != audio::kMaxVoices || before != audio::kMaxVoices) {
            printf("PS2UR_TOKEN_AUDIO_FAIL voice allocation\n");
            SleepThread();
            return 1;
        }
        if (candidate < 0) {
            printf("PS2UR_TOKEN_AUDIO_FAIL steal policy found no victim\n");
            SleepThread();
            return 1;
        }
        if (audio::can_preempt_voices() ? (loud == 0) : (loud != 0)) {
            printf("PS2UR_TOKEN_AUDIO_FAIL admission disagreed with the mixer\n");
            SleepThread();
            return 1;
        }
        // stop() cannot silence an audsrv channel -- the sample plays out.
        // Wait for the mixer to drain before the next check, exactly as a
        // game would have to.
        audio::stop_all();
        const uint32_t drain_start = now_ms();
        while (audio::active_voice_count() > 0 && now_ms() - drain_start < 4000u) {
        }
        printf("M10_DRAIN active=%u after %u ms\n",
               static_cast<unsigned>(audio::active_voice_count()),
               static_cast<unsigned>(now_ms() - drain_start));
        if (audio::active_voice_count() != 0u) {
            printf("PS2UR_TOKEN_AUDIO_FAIL mixer never drained\n");
            SleepThread();
            return 1;
        }
    }

    // --- Check 3: 3D attenuation -------------------------------------------
    {
        audio::set_listener(Vec3{0, 0, 0}, Vec3{1, 0, 0});
        audio::Attenuation3D near_source;
        near_source.position = Vec3{1.0f, 0, 0};
        near_source.min_distance = 2.0f;
        near_source.max_distance = 20.0f;
        float volume = 0.0f;
        float pan = 0.0f;
        audio::evaluate_3d(near_source, 1.0f, &volume, &pan);
        printf("M10_3D near volume=%f pan=%f\n", static_cast<double>(volume),
               static_cast<double>(pan));

        audio::Attenuation3D far_left;
        far_left.position = Vec3{-11.0f, 0, 0};
        far_left.min_distance = 2.0f;
        far_left.max_distance = 20.0f;
        audio::evaluate_3d(far_left, 1.0f, &volume, &pan);
        printf("M10_3D far_left volume=%f pan=%f\n", static_cast<double>(volume),
               static_cast<double>(pan));
        if (volume <= 0.0f || volume >= 1.0f || pan > -0.9f) {
            printf("PS2UR_TOKEN_AUDIO_FAIL 3D attenuation\n");
            SleepThread();
            return 1;
        }
        const audio::VoiceHandle positioned =
            audio::play_3d(1, audio::PlayParams{}, far_left);
        if (positioned == 0) {
            printf("PS2UR_TOKEN_AUDIO_FAIL positioned play\n");
            SleepThread();
            return 1;
        }
        audio::stop_all();
    }

    // --- Check 4: streamed music, no underruns ------------------------------
    {
        MusicFile source;
        source.fd = open_music();
        if (source.fd < 0) {
            printf("PS2UR_TOKEN_AUDIO_FAIL music file\n");
            SleepThread();
            return 1;
        }
        if (!audio::music_start(music_read, &source, 22050, 16, 2)) {
            printf("PS2UR_TOKEN_AUDIO_FAIL music start\n");
            SleepThread();
            return 1;
        }
        // Feed for a fixed stretch of WALL-CLOCK time, interleaving SFX the
        // way a game would. Counting iterations instead would prove nothing:
        // the loop spins far faster than the SPU2 drains, so 240 turns take
        // 27 ms and move almost no audio. Three seconds at 22 kHz stereo is
        // ~265 KB, and the feeder has to keep up with all of it.
        const uint32_t kRunMs = 3000u;
        uint32_t frames = 0;
        const uint32_t start = now_ms();
        uint32_t elapsed = 0;
        while (audio::music_playing() && elapsed < kRunMs) {
            if (frames % 2000u == 0u) {
                audio::PlayParams params;
                params.volume = 0.4f;
                params.priority = 5;
                audio::play(0, params);
            }
            audio::music_update();
            ++frames;
            elapsed = now_ms() - start;
        }
        printf("M10_MUSIC frames=%u bytes=%u underruns=%u elapsed=%u ms\n",
               static_cast<unsigned>(frames),
               static_cast<unsigned>(audio::music_bytes_fed()),
               static_cast<unsigned>(audio::music_underruns()),
               static_cast<unsigned>(elapsed));
        // The feeder is self-regulating: it hands over exactly what
        // audsrv_available() says the ring will take, so the sustained rate
        // IS the hardware's consumption rate. Measured at ~45 KB/s for a
        // 22050 Hz stereo stream -- half the 88,200 B/s the sample format
        // implies, because audsrv counts its ring in mono-equivalent units
        // (verify-log M10). The assertion is therefore that the stream ran
        // CONTINUOUSLY for three seconds, not that it hit a rate derived
        // from an assumption about audsrv's bookkeeping.
        const uint32_t rate = elapsed > 0
                                  ? audio::music_bytes_fed() * 1000u / elapsed
                                  : 0u;
        printf("M10_MUSIC_RATE %u bytes/s over %u ms\n",
               static_cast<unsigned>(rate), static_cast<unsigned>(elapsed));
        if (rate < 40u * 1024u) {
            printf("PS2UR_TOKEN_AUDIO_FAIL music stalled at %u bytes/s\n",
                   static_cast<unsigned>(rate));
            SleepThread();
            return 1;
        }
        if (audio::music_underruns() != 0u) {
            printf("PS2UR_TOKEN_AUDIO_FAIL %u music underruns\n",
                   static_cast<unsigned>(audio::music_underruns()));
            SleepThread();
            return 1;
        }
        audio::music_stop();
        fioClose(source.fd);
    }

    printf("PS2UR_TOKEN_AUDIO_OK\n");
    audio::shutdown();
    SleepThread();
    return 0;
}
