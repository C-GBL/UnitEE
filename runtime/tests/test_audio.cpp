// M10 task 1: voice allocation with priority stealing, and the 3D
// attenuation curve. Both are pure policy, so they run on the host without
// an SPU2 -- the on-target sample then proves the same code drives real
// hardware.
#include "ps2ur/audio.h"

#include <gtest/gtest.h>

#include <vector>

using namespace ps2ur;
using namespace ps2ur::audio;

namespace {

// A minimal valid "APCM" blob: header plus one silent ADPCM block.
std::vector<uint8_t> make_clip(uint32_t pitch = 682, uint32_t samples = 28)
{
    std::vector<uint8_t> blob;
    const char magic[4] = {'A', 'P', 'C', 'M'};
    for (char c : magic) {
        blob.push_back(static_cast<uint8_t>(c));
    }
    blob.push_back(1); // version
    blob.push_back(1); // channels
    blob.push_back(0);
    blob.push_back(0);
    for (int i = 0; i < 4; ++i) {
        blob.push_back(static_cast<uint8_t>((pitch >> (i * 8)) & 0xFF));
    }
    for (int i = 0; i < 4; ++i) {
        blob.push_back(static_cast<uint8_t>((samples >> (i * 8)) & 0xFF));
    }
    blob.resize(blob.size() + 16, 0); // one block of silence
    return blob;
}

// Every test starts from a clean mixer.
struct AudioFixture {
    std::vector<uint8_t> blob = make_clip();
    int32_t clip = -1;

    AudioFixture()
    {
        shutdown();
        init();
        clip = load_clip(blob.data(), static_cast<uint32_t>(blob.size()));
    }
    ~AudioFixture() { shutdown(); }
};

} // namespace

TEST(Audio, LoadRejectsBlobsThatAreNotApcm)
{
    AudioFixture fixture;
    ASSERT_GE(fixture.clip, 0);

    uint8_t rubbish[32] = {'N', 'O', 'P', 'E'};
    EXPECT_EQ(load_clip(rubbish, sizeof(rubbish)), -1);
    EXPECT_EQ(load_clip(nullptr, 16), -1);
    // Too short to hold a header.
    EXPECT_EQ(load_clip(rubbish, 8), -1);
}

TEST(Audio, ClipHeaderIsParsed)
{
    shutdown();
    init();
    std::vector<uint8_t> blob = make_clip(/*pitch=*/1024, /*samples=*/560);
    const int32_t clip = load_clip(blob.data(), static_cast<uint32_t>(blob.size()));
    ASSERT_GE(clip, 0);
    EXPECT_EQ(clip_info(static_cast<uint32_t>(clip)).pitch, 1024u);
    EXPECT_EQ(clip_info(static_cast<uint32_t>(clip)).sample_count, 560u);
    EXPECT_EQ(clip_info(static_cast<uint32_t>(clip)).channels, 1u);
    shutdown();
}

TEST(Audio, VoicesAreAllocatedUntilTheyRunOut)
{
    AudioFixture fixture;
    PlayParams params;
    params.priority = 10;

    std::vector<VoiceHandle> voices;
    for (uint32_t i = 0; i < kMaxVoices; ++i) {
        const VoiceHandle voice = play(static_cast<uint32_t>(fixture.clip), params);
        EXPECT_NE(voice, 0) << "voice " << i;
        voices.push_back(voice);
    }
    EXPECT_EQ(active_voice_count(), kMaxVoices);

    // The 25th sound is dropped rather than cutting one short.
    EXPECT_EQ(play(static_cast<uint32_t>(fixture.clip), params), 0);
    EXPECT_EQ(dropped_count(), 1u);
}

TEST(Audio, TheStealPolicyPicksTheWeakestSoundingVoice)
{
    AudioFixture fixture;
    // Fill with a mix of priorities; voice 3 is the weakest.
    std::vector<VoiceHandle> voices;
    for (uint32_t i = 0; i < kMaxVoices; ++i) {
        PlayParams params;
        params.priority = (i == 3u) ? 1 : 50;
        voices.push_back(play(static_cast<uint32_t>(fixture.clip), params));
    }
    ASSERT_EQ(active_voice_count(), kMaxVoices);

    // The POLICY is testable even where the mixer cannot act on it.
    EXPECT_EQ(steal_candidate_voice(100), 3);
    // Nothing sounding is weaker than priority 1, so nothing is sacrificed.
    EXPECT_EQ(steal_candidate_voice(1), -1);
    EXPECT_EQ(steal_candidate_voice(0), -1);
}

TEST(Audio, WithoutPreemptionAFullMixerDropsTheNewSound)
{
    // audsrv cannot stop a sounding ADPCM channel, so a sound arriving when
    // all 24 voices are busy is DROPPED, however important it is. The host
    // build honours the same rule on purpose: a host-only steal would let
    // this test assert behaviour the console cannot deliver (verify-log M10).
    ASSERT_FALSE(can_preempt_voices());

    AudioFixture fixture;
    PlayParams quiet;
    quiet.priority = 1;
    std::vector<VoiceHandle> voices;
    for (uint32_t i = 0; i < kMaxVoices; ++i) {
        const VoiceHandle voice = play(static_cast<uint32_t>(fixture.clip), quiet);
        ASSERT_NE(voice, 0);
        voices.push_back(voice);
    }
    PlayParams important;
    important.priority = 1000;
    EXPECT_EQ(play(static_cast<uint32_t>(fixture.clip), important), 0);
    EXPECT_EQ(dropped_count(), 1u);
    EXPECT_EQ(active_voice_count(), kMaxVoices);

    // Freeing one voice lets the next important sound in. (Stopping needs a
    // real handle: handles carry a generation, so a bare index is rejected.)
    stop(voices[0]);
    EXPECT_NE(play(static_cast<uint32_t>(fixture.clip), important), 0);
}

TEST(Audio, StoppingFreesTheVoiceAndRetiresTheHandle)
{
    AudioFixture fixture;
    PlayParams params;
    const VoiceHandle voice = play(static_cast<uint32_t>(fixture.clip), params);
    ASSERT_NE(voice, 0);
    EXPECT_EQ(active_voice_count(), 1u);

    stop(voice);
    EXPECT_EQ(active_voice_count(), 0u);
    EXPECT_FALSE(is_playing(voice));
    stop(voice); // stopping twice is harmless
}

TEST(Audio, LinearRolloffMatchesUnitySemantics)
{
    shutdown();
    init();
    set_listener(Vec3{0, 0, 0}, Vec3{1, 0, 0});

    Attenuation3D attenuation;
    attenuation.min_distance = 2.0f;
    attenuation.max_distance = 12.0f;

    float volume = 0.0f;
    float pan = 0.0f;

    // Inside the minimum distance: full volume.
    attenuation.position = Vec3{1.0f, 0, 0};
    evaluate_3d(attenuation, 1.0f, &volume, &pan);
    EXPECT_NEAR(volume, 1.0f, 1e-5f);

    // Exactly halfway along the rolloff span.
    attenuation.position = Vec3{7.0f, 0, 0};
    evaluate_3d(attenuation, 1.0f, &volume, &pan);
    EXPECT_NEAR(volume, 0.5f, 1e-5f);

    // Past the maximum: silent.
    attenuation.position = Vec3{20.0f, 0, 0};
    evaluate_3d(attenuation, 1.0f, &volume, &pan);
    EXPECT_NEAR(volume, 0.0f, 1e-5f);

    // Source volume scales the result.
    attenuation.position = Vec3{7.0f, 0, 0};
    evaluate_3d(attenuation, 0.5f, &volume, &pan);
    EXPECT_NEAR(volume, 0.25f, 1e-5f);
    shutdown();
}

TEST(Audio, PanFollowsTheListenerRightVector)
{
    shutdown();
    init();
    set_listener(Vec3{0, 0, 0}, Vec3{1, 0, 0});

    Attenuation3D attenuation;
    attenuation.min_distance = 100.0f; // keep volume out of the way
    attenuation.max_distance = 200.0f;
    float volume = 0.0f;
    float pan = 0.0f;

    attenuation.position = Vec3{5.0f, 0, 0}; // hard right
    evaluate_3d(attenuation, 1.0f, &volume, &pan);
    EXPECT_NEAR(pan, 1.0f, 1e-4f);

    attenuation.position = Vec3{-5.0f, 0, 0}; // hard left
    evaluate_3d(attenuation, 1.0f, &volume, &pan);
    EXPECT_NEAR(pan, -1.0f, 1e-4f);

    attenuation.position = Vec3{0, 0, 5.0f}; // straight ahead
    evaluate_3d(attenuation, 1.0f, &volume, &pan);
    EXPECT_NEAR(pan, 0.0f, 1e-4f);

    // Turning the listener turns the panning with it.
    set_listener(Vec3{0, 0, 0}, Vec3{0, 0, 1});
    attenuation.position = Vec3{0, 0, 5.0f};
    evaluate_3d(attenuation, 1.0f, &volume, &pan);
    EXPECT_NEAR(pan, 1.0f, 1e-4f);
    shutdown();
}

TEST(Audio, OutOfRangeSourcesNeverSpendAVoice)
{
    AudioFixture fixture;
    set_listener(Vec3{0, 0, 0}, Vec3{1, 0, 0});
    Attenuation3D attenuation;
    attenuation.min_distance = 1.0f;
    attenuation.max_distance = 10.0f;
    attenuation.position = Vec3{500.0f, 0, 0};

    PlayParams params;
    EXPECT_EQ(play_3d(static_cast<uint32_t>(fixture.clip), params, attenuation), 0);
    EXPECT_EQ(active_voice_count(), 0u);
}

namespace {

// A music source that yields a fixed number of chunks then runs dry.
struct MusicSource {
    uint32_t chunks_left;
    uint32_t calls = 0;
};

uint32_t music_source_read(void* user, void* dest, uint32_t bytes)
{
    MusicSource* source = static_cast<MusicSource*>(user);
    ++source->calls;
    if (source->chunks_left == 0) {
        return 0;
    }
    --source->chunks_left;
    for (uint32_t i = 0; i < bytes; ++i) {
        static_cast<uint8_t*>(dest)[i] = static_cast<uint8_t>(i);
    }
    return bytes;
}

} // namespace

TEST(Audio, MusicPrimesTwoChunksAndStopsWhenTheSourceRunsDry)
{
    shutdown();
    init();
    MusicSource source{/*chunks_left=*/4};

    // Priming fills the ring before playback so the first frames are never a
    // race between the feeder and the hardware; it stops early if the source
    // runs dry, which is what happens here (4 chunks available).
    ASSERT_TRUE(music_start(music_source_read, &source, 22050, 16, 2));
    EXPECT_EQ(source.chunks_left, 0u);
    EXPECT_GT(music_bytes_fed(), 0u);
    EXPECT_EQ(music_underruns(), 0u);

    // The next call gets nothing: the stream ends cleanly rather than
    // looping stale audio.
    EXPECT_FALSE(music_update());
    EXPECT_FALSE(music_playing());
    shutdown();
}

TEST(Audio, MusicUpdateIsSafeAfterStopping)
{
    shutdown();
    init();
    MusicSource source{/*chunks_left=*/1};
    ASSERT_TRUE(music_start(music_source_read, &source, 22050, 16, 2));
    music_stop();
    EXPECT_FALSE(music_playing());
    EXPECT_FALSE(music_update());
    shutdown();
}
