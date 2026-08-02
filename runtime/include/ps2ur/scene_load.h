// Scene loading: synchronous, asynchronous and additive (plan section 9,
// M10 task 5).
//
// Async loading is built on the stream queue, so a load shares the frame
// with the music feeder and a spinning loading screen instead of freezing
// the console. The managed side drives it from a coroutine: start a load,
// yield until progress reaches 1, then swap.
//
// ADDITIVE loading appends a second .p2b into a world that is already
// running. Its entity indices are rebased, which is why every reference
// inside a .p2b is relative to the file rather than global.
#pragma once

#include "ps2ur/p2b.h"
#include "ps2ur/p2b_scene.h"

#include <cstdint>

namespace ps2ur {
namespace scene {

enum class LoadState : uint8_t {
    Idle = 0,
    Reading,   // the container is still coming off the media
    Parsing,   // read complete; building the world
    Ready,     // world usable
    Failed,
};

// A load in flight. The destination buffer must outlive the world, because
// meshes and clips point straight into it (zero copy).
class SceneLoader {
public:
    // Starts an asynchronous load. 'buffer' receives the raw container.
    // additive == false replaces the world's contents when it completes.
    bool begin(const char* path, void* buffer, uint32_t capacity, World* world,
               bool additive);

    // Advances the load by a bounded amount. Call once a frame. Returns the
    // current state; Ready and Failed are terminal.
    LoadState update(uint32_t byte_budget);

    // Loads everything in one go -- for a boot scene, where there is no
    // frame to share with anyway.
    bool load_blocking(const char* path, void* buffer, uint32_t capacity,
                       World* world, bool additive);

    // Holding activation off stops the load at Parsing once the read
    // completes -- progress parks at 0.9 and the world is NOT swapped until
    // activation is granted. This is Unity's
    // AsyncOperation.allowSceneActivation, and it exists so a game can
    // finish a fade or a transition before the scene changes underneath it.
    // Defaults to true (begin() resets it), matching Unity.
    void set_allow_activation(bool allow) { m_allow_activation = allow; }
    bool allow_activation() const { return m_allow_activation; }

    LoadState state() const { return m_state; }
    // 0..1 across read and parse together, which is what a loading bar wants.
    float progress() const;
    const char* error() const { return m_error; }
    uint32_t bytes_read() const { return m_bytes; }

private:
    bool finish_parse();

    LoadState m_state = LoadState::Idle;
    uint32_t m_request = 0;
    uint8_t* m_buffer = nullptr;
    uint32_t m_capacity = 0;
    uint32_t m_bytes = 0;
    World* m_world = nullptr;
    bool m_additive = false;
    bool m_allow_activation = true;
    const char* m_error = "";
    io::P2bFile m_file;
};

} // namespace scene
} // namespace ps2ur
