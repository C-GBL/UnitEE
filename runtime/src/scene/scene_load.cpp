#include "ps2ur/scene_load.h"

#include "ps2ur/log.h"
#include "ps2ur/stream.h"

namespace ps2ur {
namespace scene {

namespace {

// Reading dominates a load; parsing is a single pass over already-resident
// memory. Splitting the bar 90/10 keeps it honest rather than stalling at
// 100% while the parse runs.
constexpr float kReadShare = 0.9f;

} // namespace

bool SceneLoader::begin(const char* path, void* buffer, uint32_t capacity,
                        World* world, bool additive)
{
    if (path == nullptr || buffer == nullptr || world == nullptr ||
        capacity == 0) {
        m_error = "bad arguments";
        m_state = LoadState::Failed;
        return false;
    }
    if (!stream::initialized()) {
        stream::init();
    }
    m_buffer = static_cast<uint8_t*>(buffer);
    m_capacity = capacity;
    m_world = world;
    m_additive = additive;
    m_allow_activation = true;
    m_bytes = 0;
    m_error = "";

    // Normal priority: a level load must not out-rank an audio refill, or
    // the music stutters for the whole loading screen.
    m_request = stream::request(path, m_buffer, m_capacity,
                                stream::Priority::Normal, nullptr, nullptr);
    if (m_request == 0) {
        m_error = "stream queue full";
        m_state = LoadState::Failed;
        return false;
    }
    m_state = LoadState::Reading;
    return true;
}

LoadState SceneLoader::update(uint32_t byte_budget)
{
    if (m_state == LoadState::Parsing) {
        // Read done, activation withheld: parked at 0.9 until granted.
        if (!m_allow_activation) {
            return m_state;
        }
        return finish_parse() ? LoadState::Ready : LoadState::Failed;
    }
    if (m_state != LoadState::Reading) {
        return m_state;
    }
    stream::update(byte_budget);
    m_bytes = stream::bytes_transferred(m_request);

    switch (stream::state(m_request)) {
        case stream::RequestState::Done:
            m_state = LoadState::Parsing;
            if (!m_allow_activation) {
                return m_state;
            }
            return finish_parse() ? LoadState::Ready : LoadState::Failed;
        case stream::RequestState::Failed:
            m_error = "read failed";
            m_state = LoadState::Failed;
            return m_state;
        case stream::RequestState::Cancelled:
            m_error = "cancelled";
            m_state = LoadState::Failed;
            return m_state;
        default:
            return m_state;
    }
}

bool SceneLoader::finish_parse()
{
    if (!m_file.parse(m_buffer, m_bytes)) {
        m_error = m_file.error();
        m_state = LoadState::Failed;
        return false;
    }
    const bool ok = m_additive ? m_world->append(m_file) : m_world->load(m_file);
    if (!ok) {
        m_error = m_world->error();
        m_state = LoadState::Failed;
        return false;
    }
    m_state = LoadState::Ready;
    return true;
}

bool SceneLoader::load_blocking(const char* path, void* buffer,
                                uint32_t capacity, World* world, bool additive)
{
    if (!begin(path, buffer, capacity, world, additive)) {
        return false;
    }
    uint32_t guard = 0;
    while (m_state == LoadState::Reading && guard++ < 100000u) {
        update(64 * 1024);
    }
    return m_state == LoadState::Ready;
}

float SceneLoader::progress() const
{
    switch (m_state) {
        case LoadState::Idle:
            return 0.0f;
        case LoadState::Ready:
            return 1.0f;
        case LoadState::Failed:
            return 1.0f; // the bar stops moving; the caller reads state()
        case LoadState::Parsing:
            return kReadShare;
        default:
            break;
    }
    // Reading: the queue's own fraction, scaled into the read share.
    return stream::progress() * kReadShare;
}

} // namespace scene
} // namespace ps2ur
