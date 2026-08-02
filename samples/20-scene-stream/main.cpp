// samples/20-scene-stream -- the M10 ACCEPTANCE run (plan section 9): load a
// second scene while music plays, with no audio dropouts, inside the time
// budget. Task 5 (async + additive scene loading) meets task 1 (streamed
// music) and task 4 (the prioritised queue) here, because the thing that
// actually breaks on a console is the interaction: a level load that hogs
// the frame starves the music feeder and the track stutters.
//
// The load is deliberately driven the way the managed coroutine drives it --
// begin(), then a bounded update() once per frame with the music fed in
// between -- so this sample is the native mirror of what
// SceneManager.LoadSceneAsync does through the bridge.
//
// What PCSX2 can prove: that the interleaving works, that progress advances
// monotonically, that the world swaps correctly, and that the feeder never
// underruns while a load is in flight. What it CANNOT prove is CDVD seek
// timing -- host: reads have no seek cost at all, so the <=8 s budget in the
// plan is only meaningful once this runs from a real disc. The elapsed time
// is reported rather than asserted tightly for exactly that reason.
#include <ps2ur/audio.h>
#include <ps2ur/log.h>
#include <ps2ur/p2b_scene.h>
#include <ps2ur/platform.h>
#include <ps2ur/scene_load.h>
#include <ps2ur/stream.h>

#include <kernel.h>
#include <stdio.h>

using namespace ps2ur;

extern "C" {
int fioOpen(const char* name, int mode);
int fioClose(int fd);
int fioRead(int fd, void* buf, int size);
}

namespace {

// Scene containers land here; the world points straight into these buffers,
// so they must outlive it. Each additive load therefore needs its OWN
// buffer -- reusing one would pull the meshes out from under the scene that
// is already merged and running.
alignas(16) uint8_t g_scene_a[1024 * 1024];
alignas(16) uint8_t g_scene_b[1024 * 1024];
alignas(16) uint8_t g_scene_c[256 * 1024];

scene::World g_world;

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
            return fd;
        }
    }
    return -1;
}

void fail(const char* what)
{
    printf("PS2UR_TOKEN_SCENESTREAM_FAIL %s\n", what);
}

} // namespace

int main(void)
{
    platform::init();
    if (!stream::init()) {
        fail("stream init");
        SleepThread();
        return 1;
    }
    if (!audio::init()) {
        fail("audio init");
        SleepThread();
        return 1;
    }

    // --- The boot scene, loaded the blocking way ----------------------------
    {
        scene::SceneLoader boot;
        if (!boot.load_blocking("host:m5-scene.p2b", g_scene_a,
                                sizeof(g_scene_a), &g_world, false)) {
            fail(boot.error());
            SleepThread();
            return 1;
        }
        printf("[20] boot scene: %u entities, %u meshes\n",
               static_cast<unsigned>(g_world.entity_count()),
               static_cast<unsigned>(g_world.mesh_count()));
    }
    const uint32_t entities_before = g_world.entity_count();

    // --- Music starts and must not stop for the rest of the run -------------
    MusicFile source;
    source.fd = open_music();
    if (source.fd < 0) {
        fail("music file");
        SleepThread();
        return 1;
    }
    if (!audio::music_start(music_read, &source, 22050, 16, 2)) {
        fail("music start");
        SleepThread();
        return 1;
    }

    // --- Scene 2, loaded asynchronously with music playing ------------------
    //
    // 64 KB per frame is the same budget SceneManager uses on the managed
    // side, so the pacing here is the pacing a game gets.
    // skinscene is the biggest container the exporter produces (430 KB) and
    // it carries characters, so this exercises the animation side of the
    // merge rather than just geometry.
    scene::SceneLoader loader;
    if (!loader.begin("host:skinscene.p2b", g_scene_b, sizeof(g_scene_b),
                      &g_world, /*additive=*/true)) {
        fail(loader.error());
        SleepThread();
        return 1;
    }

    uint32_t frames = 0;
    uint32_t worst_frame_ms = 0;
    uint32_t read_ms = 0;
    uint32_t parse_ms = 0;
    float last_progress = 0.0f;
    bool progress_went_backwards = false;
    const uint32_t start = now_ms();
    while (loader.state() == scene::LoadState::Reading && frames < 20000u) {
        const uint32_t frame_start = now_ms();

        // Exactly the managed frame order: advance the load, then feed the
        // mixer. Music LAST is the harder ordering -- if the load overruns
        // its budget, the feeder is what pays for it.
        const uint32_t before_update = now_ms();
        const scene::LoadState after = loader.update(64 * 1024);
        const uint32_t update_ms = now_ms() - before_update;
        // The call that leaves Reading is the one that also parsed, so this
        // separates "time spent reading" from "time spent building the
        // world" -- which is the number that decides whether the parse needs
        // to be incremental too.
        if (after == scene::LoadState::Reading) {
            read_ms += update_ms;
        } else {
            parse_ms = update_ms;
        }
        audio::music_update();

        const float p = loader.progress();
        if (p < last_progress - 0.001f) {
            progress_went_backwards = true;
        }
        last_progress = p;

        const uint32_t frame_ms = now_ms() - frame_start;
        if (frame_ms > worst_frame_ms) {
            worst_frame_ms = frame_ms;
        }
        ++frames;
    }
    const uint32_t elapsed = now_ms() - start;

    printf("M10_SCENELOAD state=%d frames=%u elapsed=%u ms worst_frame=%u ms "
           "read=%u ms parse=%u ms progress=%.3f\n",
           static_cast<int>(loader.state()), static_cast<unsigned>(frames),
           static_cast<unsigned>(elapsed),
           static_cast<unsigned>(worst_frame_ms),
           static_cast<unsigned>(read_ms), static_cast<unsigned>(parse_ms),
           static_cast<double>(loader.progress()));
    printf("M10_SCENELOAD_MUSIC bytes=%u underruns=%u playing=%d\n",
           static_cast<unsigned>(audio::music_bytes_fed()),
           static_cast<unsigned>(audio::music_underruns()),
           audio::music_playing() ? 1 : 0);
    printf("M10_SCENELOAD_WORLD before=%u after=%u meshes=%u skinned=%u "
           "clips=%u\n",
           static_cast<unsigned>(entities_before),
           static_cast<unsigned>(g_world.entity_count()),
           static_cast<unsigned>(g_world.mesh_count()),
           static_cast<unsigned>(g_world.skinned_renderer_count()),
           static_cast<unsigned>(g_world.clip_count()));

    if (loader.state() != scene::LoadState::Ready) {
        fail(loader.error());
        SleepThread();
        return 1;
    }
    if (frames < 2u) {
        // One-gulp loading is not asynchronous loading; the music feeder
        // would never get a turn.
        fail("the load did not span multiple frames");
        SleepThread();
        return 1;
    }
    if (progress_went_backwards) {
        fail("progress went backwards");
        SleepThread();
        return 1;
    }
    if (g_world.entity_count() <= entities_before) {
        fail("additive load added nothing");
        SleepThread();
        return 1;
    }
    // The characters have to survive the merge with working animators; a
    // scene that arrives and then stands still is the failure this catches.
    if (g_world.skinned_renderer_count() == 0u) {
        fail("additive load dropped the skinned characters");
        SleepThread();
        return 1;
    }
    for (uint32_t i = 0; i < g_world.skinned_renderer_count(); ++i) {
        if (!g_world.animator(g_world.skinned_renderer(i).animator).valid()) {
            fail("a merged character has no bound animator");
            SleepThread();
            return 1;
        }
    }
    if (audio::music_underruns() != 0u) {
        fail("music underran during the load");
        SleepThread();
        return 1;
    }
    if (!audio::music_playing()) {
        fail("music stopped during the load");
        SleepThread();
        return 1;
    }
    // The acceptance budget. host: has no seek cost, so passing this here is
    // necessary but nowhere near sufficient -- see the header comment.
    if (elapsed > 8000u) {
        fail("load exceeded the 8 s budget");
        SleepThread();
        return 1;
    }

    // --- Withheld activation (AsyncOperation.allowSceneActivation) ----------
    //
    // A game fading out before a scene change needs the read to finish while
    // the swap waits. Held, the world must not change.
    {
        const uint32_t before = g_world.entity_count();
        scene::SceneLoader held;
        if (!held.begin("host:spin-scene.p2b", g_scene_c, sizeof(g_scene_c),
                        &g_world, true)) {
            fail(held.error());
            SleepThread();
            return 1;
        }
        held.set_allow_activation(false);
        uint32_t guard = 0;
        while (held.state() == scene::LoadState::Reading && guard++ < 20000u) {
            held.update(64 * 1024);
            audio::music_update();
        }
        printf("M10_SCENELOAD_HOLD state=%d progress=%.3f entities=%u\n",
               static_cast<int>(held.state()),
               static_cast<double>(held.progress()),
               static_cast<unsigned>(g_world.entity_count()));
        if (held.state() != scene::LoadState::Parsing) {
            fail("withheld activation did not park the load");
            SleepThread();
            return 1;
        }
        if (g_world.entity_count() != before) {
            fail("the world swapped while activation was withheld");
            SleepThread();
            return 1;
        }
        held.set_allow_activation(true);
        held.update(64 * 1024);
        printf("M10_SCENELOAD_ACTIVATE state=%d entities=%u->%u error='%s'\n",
               static_cast<int>(held.state()), static_cast<unsigned>(before),
               static_cast<unsigned>(g_world.entity_count()), held.error());
        if (held.state() != scene::LoadState::Ready ||
            g_world.entity_count() <= before) {
            fail("granting activation did not finish the load");
            SleepThread();
            return 1;
        }
    }

    // --- The first-access trace, for the disc-layout planner ----------------
    printf("M10_TRACE %u entries:\n",
           static_cast<unsigned>(stream::trace_count()));
    for (uint32_t i = 0; i < stream::trace_count(); ++i) {
        printf("M10_TRACE   %u %s\n", static_cast<unsigned>(i),
               stream::trace_entry(i));
    }

    printf("PS2UR_TOKEN_SCENESTREAM_OK\n");
    audio::music_stop();
    fioClose(source.fd);
    audio::shutdown();
    stream::shutdown();
    SleepThread();
    return 0;
}
