// Audio half of the managed<->native boundary (M12.5 task 2).
//
// The M10 mixer thinks in VOICES; Unity thinks in AudioSources. The boundary
// speaks voices -- a managed AudioSource owns at most one main voice handle
// and fires one-shots it never tracks -- because that keeps the native side
// free of any per-source object whose lifetime managed code would have to
// manage across the boundary. A handle of 0 means "no voice": that is what
// the mixer returns when every voice is busy with something more important,
// and a dropped sound is normal, never an error.
//
// Looping is a property of the CLIP, not the call: SPU2 ADPCM loop flags are
// baked into the sample stream at encode. The exporter encodes a clip looped
// when the source that references it loops.
//
// The one thing that cannot be per-call is SPATIALISATION: a 3D voice's pan
// and attenuation follow its entity and the listener every frame. Managed
// code calling per-source-per-frame would be the interop shape M7 measured
// and rejected, so spatial voices register HERE, and audio_frame_update()
// -- called once per frame by the host after the managed tick -- walks the
// table natively: listener from the listener entity's world matrix, then
// evaluate_3d + set_voice_volume per live voice.
#include "ps2ur/bridge.h"

#include "ps2ur/audio.h"
#include "ps2ur/p2b_scene.h"

#include "generated_bridge.h"

namespace {

using namespace ps2ur;

struct SpatialVoice {
    audio::VoiceHandle voice = 0;
    int32_t entity_handle = 0;
    float base_volume = 1.0f;
    audio::Attenuation3D attenuation;
    bool live = false;
};

// One slot per SPU2 voice is the natural bound.
SpatialVoice g_spatial[24];
int32_t g_listener_handle = 0;

void register_spatial(audio::VoiceHandle voice, int32_t entity_handle,
                      float volume, const audio::Attenuation3D& attenuation)
{
    for (SpatialVoice& s : g_spatial) {
        if (s.live && s.voice == voice) {
            s.live = false; // the mixer reused a voice we were tracking
        }
    }
    for (SpatialVoice& s : g_spatial) {
        if (!s.live) {
            s.voice = voice;
            s.entity_handle = entity_handle;
            s.base_volume = volume;
            s.attenuation = attenuation;
            s.live = true;
            return;
        }
    }
}

} // namespace

namespace ps2ur {
namespace bridge {

void audio_set_listener_handle(int32_t entity_handle)
{
    g_listener_handle = entity_handle;
}

// Once per frame, after the managed tick moved everything: listener pose,
// then pan/attenuation for every live spatial voice.
void audio_frame_update()
{
    scene::World* world = bridge::world();
    if (world == nullptr || !audio::initialized()) {
        return;
    }
    if (g_listener_handle != 0) {
        const int32_t listener = world->resolve(g_listener_handle);
        if (listener >= 0) {
            const Mat4& w = world->world_matrix(static_cast<uint32_t>(listener));
            audio::set_listener(Vec3{w.m[12], w.m[13], w.m[14]},
                                Vec3{w.m[0], w.m[1], w.m[2]});
        }
    }
    for (SpatialVoice& s : g_spatial) {
        if (!s.live) {
            continue;
        }
        if (!audio::is_playing(s.voice)) {
            s.live = false;
            continue;
        }
        const int32_t entity = world->resolve(s.entity_handle);
        if (entity < 0) {
            // The entity died; the sound plays out as it was rather than
            // popping.
            s.live = false;
            continue;
        }
        const Mat4& w = world->world_matrix(static_cast<uint32_t>(entity));
        s.attenuation.position = Vec3{w.m[12], w.m[13], w.m[14]};
        float volume = 0.0f;
        float pan = 0.0f;
        audio::evaluate_3d(s.attenuation, s.base_volume, &volume, &pan);
        audio::set_voice_volume(s.voice, volume, pan);
    }
}

} // namespace bridge
} // namespace ps2ur

// ---- C surface (bridge-api.json owns the signatures) -----------------------

extern "C" int32_t ps2ur_audio_play(int32_t entity_handle, int32_t clip,
                                    float volume, int32_t priority,
                                    int32_t spatial, float min_distance,
                                    float max_distance)
{
    if (!audio::initialized() || clip < 0 ||
        static_cast<uint32_t>(clip) >= audio::clip_count()) {
        return 0;
    }
    audio::PlayParams params;
    params.volume = volume;
    params.pan = 0.0f;
    params.priority = priority;
    if (spatial == 0) {
        return audio::play(static_cast<uint32_t>(clip), params);
    }
    audio::Attenuation3D attenuation;
    attenuation.min_distance = min_distance;
    attenuation.max_distance = max_distance;
    scene::World* world = bridge::world();
    const int32_t entity =
        world != nullptr ? world->resolve(entity_handle) : -1;
    if (entity >= 0) {
        const Mat4& w = world->world_matrix(static_cast<uint32_t>(entity));
        attenuation.position = Vec3{w.m[12], w.m[13], w.m[14]};
    }
    const audio::VoiceHandle voice =
        audio::play_3d(static_cast<uint32_t>(clip), params, attenuation);
    if (voice > 0) {
        register_spatial(voice, entity_handle, volume, attenuation);
    }
    return voice;
}

extern "C" void ps2ur_audio_stop(int32_t voice)
{
    if (audio::initialized() && voice > 0) {
        audio::stop(voice);
    }
}

extern "C" int32_t ps2ur_audio_is_playing(int32_t voice)
{
    return audio::initialized() && voice > 0 && audio::is_playing(voice) ? 1
                                                                         : 0;
}

extern "C" void ps2ur_audio_set_volume(int32_t voice, float volume)
{
    if (!audio::initialized() || voice <= 0) {
        return;
    }
    // A spatial voice's change lands through its table entry, so the frame
    // update keeps owning pan; 2D voices sit centred.
    for (SpatialVoice& s : g_spatial) {
        if (s.live && s.voice == voice) {
            s.base_volume = volume;
            return;
        }
    }
    audio::set_voice_volume(voice, volume, 0.0f);
}

extern "C" int32_t ps2ur_audio_clip_count()
{
    return audio::initialized() ? static_cast<int32_t>(audio::clip_count()) : 0;
}

extern "C" float ps2ur_audio_clip_seconds(int32_t clip)
{
    if (!audio::initialized() || clip < 0 ||
        static_cast<uint32_t>(clip) >= audio::clip_count()) {
        return 0.0f;
    }
    const audio::ClipInfo& info = audio::clip_info(static_cast<uint32_t>(clip));
    // pitch = freq * 4096 / 48000, so freq = pitch * 48000 / 4096.
    const float freq = static_cast<float>(info.pitch) * 48000.0f / 4096.0f;
    return freq > 0.0f ? static_cast<float>(info.sample_count) / freq : 0.0f;
}
