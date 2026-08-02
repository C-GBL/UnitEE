// M10 task 4: the prioritised read queue, its progress accounting, and the
// first-access recorder the disc-layout planner consumes. Real files on the
// host, so the queue is exercised end to end rather than against a mock.
#include "ps2ur/stream.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace ps2ur;
using namespace ps2ur::stream;

namespace {

// Writes a temp file of 'size' bytes with a recognisable pattern.
std::string make_file(const char* name, uint32_t size, uint8_t seed)
{
    std::string path = std::string(std::tmpnam(nullptr)) + name;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    EXPECT_NE(f, nullptr) << path;
    for (uint32_t i = 0; i < size; ++i) {
        const uint8_t byte = static_cast<uint8_t>(seed + (i & 0xFF));
        std::fputc(byte, f);
    }
    std::fclose(f);
    return path;
}

struct StreamFixture {
    StreamFixture()
    {
        shutdown();
        init();
        trace_reset();
    }
    ~StreamFixture()
    {
        shutdown();
        trace_reset();
    }
};

struct Completion {
    uint32_t calls = 0;
    uint32_t last_request = 0;
    bool last_ok = false;
    uint32_t last_bytes = 0;
    std::vector<uint32_t> order;
};

void on_complete(void* user, uint32_t request, bool ok, uint32_t bytes)
{
    Completion* c = static_cast<Completion*>(user);
    ++c->calls;
    c->last_request = request;
    c->last_ok = ok;
    c->last_bytes = bytes;
    c->order.push_back(request);
}

} // namespace

TEST(Stream, ReadsAWholeFileInBoundedSteps)
{
    StreamFixture fixture;
    const std::string path = make_file("a.bin", 100 * 1024, 1);

    std::vector<uint8_t> buffer(200 * 1024);
    Completion done;
    const uint32_t id = request(path.c_str(), buffer.data(),
                                static_cast<uint32_t>(buffer.size()),
                                Priority::Normal, on_complete, &done);
    ASSERT_NE(id, 0u);
    EXPECT_EQ(pending(), 1u);

    // A single small update must NOT finish a 100 KB file: the point of the
    // queue is that it never disappears into the drive for a whole frame.
    update(16 * 1024);
    EXPECT_EQ(pending(), 1u);
    EXPECT_LT(bytes_transferred(id), 100u * 1024u);

    uint32_t guard = 0;
    while (pending() > 0 && guard++ < 1000) {
        update(16 * 1024);
    }
    EXPECT_EQ(state(id), RequestState::Done);
    EXPECT_EQ(bytes_transferred(id), 100u * 1024u);
    EXPECT_EQ(done.calls, 1u);
    EXPECT_TRUE(done.last_ok);

    // Content actually arrived.
    EXPECT_EQ(buffer[0], 1);
    EXPECT_EQ(buffer[5], 6);
    std::remove(path.c_str());
}

TEST(Stream, HighPriorityJumpsTheQueue)
{
    StreamFixture fixture;
    const std::string big = make_file("big.bin", 64 * 1024, 10);
    const std::string small = make_file("small.bin", 1024, 20);

    std::vector<uint8_t> big_buffer(128 * 1024);
    std::vector<uint8_t> small_buffer(8 * 1024);
    Completion done;

    // The big low-priority read is submitted FIRST...
    const uint32_t low = request(big.c_str(), big_buffer.data(),
                                 static_cast<uint32_t>(big_buffer.size()),
                                 Priority::Low, on_complete, &done);
    // ...but an audio refill arrives behind it and must not wait.
    const uint32_t high = request(small.c_str(), small_buffer.data(),
                                  static_cast<uint32_t>(small_buffer.size()),
                                  Priority::High, on_complete, &done);
    ASSERT_NE(low, 0u);
    ASSERT_NE(high, 0u);

    uint32_t guard = 0;
    while (pending() > 0 && guard++ < 1000) {
        update(4 * 1024);
    }
    ASSERT_EQ(done.order.size(), 2u);
    EXPECT_EQ(done.order[0], high) << "high priority must finish first";
    EXPECT_EQ(done.order[1], low);

    std::remove(big.c_str());
    std::remove(small.c_str());
}

TEST(Stream, EqualPrioritiesKeepSubmissionOrder)
{
    StreamFixture fixture;
    const std::string first = make_file("f1.bin", 2048, 1);
    const std::string second = make_file("f2.bin", 2048, 2);

    std::vector<uint8_t> b1(4096), b2(4096);
    Completion done;
    const uint32_t a = request(first.c_str(), b1.data(), 4096,
                               Priority::Normal, on_complete, &done);
    const uint32_t b = request(second.c_str(), b2.data(), 4096,
                               Priority::Normal, on_complete, &done);
    drain(nullptr, nullptr);
    ASSERT_EQ(done.order.size(), 2u);
    EXPECT_EQ(done.order[0], a);
    EXPECT_EQ(done.order[1], b);

    std::remove(first.c_str());
    std::remove(second.c_str());
}

TEST(Stream, MissingFileFailsTheRequestNotTheQueue)
{
    StreamFixture fixture;
    std::vector<uint8_t> buffer(1024);
    Completion done;
    const uint32_t id = request("definitely/not/here.bin", buffer.data(), 1024,
                                Priority::Normal, on_complete, &done);
    ASSERT_NE(id, 0u);
    drain(nullptr, nullptr);
    EXPECT_EQ(state(id), RequestState::Failed);
    EXPECT_EQ(done.calls, 1u);
    EXPECT_FALSE(done.last_ok);
    EXPECT_EQ(pending(), 0u);
}

TEST(Stream, AFileLargerThanTheBufferIsRefusedRatherThanOverrunning)
{
    StreamFixture fixture;
    const std::string path = make_file("huge.bin", 32 * 1024, 3);
    std::vector<uint8_t> small(1024);
    Completion done;
    const uint32_t id = request(path.c_str(), small.data(), 1024,
                                Priority::Normal, on_complete, &done);
    drain(nullptr, nullptr);
    EXPECT_EQ(state(id), RequestState::Failed);
    EXPECT_EQ(bytes_transferred(id), 0u);
    std::remove(path.c_str());
}

TEST(Stream, ProgressRunsZeroToOneAndResetsBetweenBatches)
{
    StreamFixture fixture;
    const std::string a = make_file("p1.bin", 4096, 1);
    const std::string b = make_file("p2.bin", 4096, 2);
    std::vector<uint8_t> b1(8192), b2(8192);

    request(a.c_str(), b1.data(), 8192, Priority::Normal, nullptr, nullptr);
    request(b.c_str(), b2.data(), 8192, Priority::Normal, nullptr, nullptr);
    EXPECT_FLOAT_EQ(progress(), 0.0f);

    drain(nullptr, nullptr);
    EXPECT_FLOAT_EQ(progress(), 1.0f);

    // A second load starts its bar from zero rather than continuing.
    request(a.c_str(), b1.data(), 8192, Priority::Normal, nullptr, nullptr);
    EXPECT_FLOAT_EQ(progress(), 0.0f);
    drain(nullptr, nullptr);

    std::remove(a.c_str());
    std::remove(b.c_str());
}

TEST(Stream, CancelStopsAReadAndFreesTheSlot)
{
    StreamFixture fixture;
    const std::string path = make_file("c.bin", 128 * 1024, 7);
    std::vector<uint8_t> buffer(256 * 1024);
    Completion done;
    const uint32_t id = request(path.c_str(), buffer.data(),
                                static_cast<uint32_t>(buffer.size()),
                                Priority::Normal, on_complete, &done);
    update(4096);
    cancel(id);
    EXPECT_EQ(state(id), RequestState::Cancelled);
    EXPECT_EQ(pending(), 0u);
    EXPECT_EQ(done.calls, 1u);
    EXPECT_FALSE(done.last_ok);
    std::remove(path.c_str());
}

TEST(Stream, TheQueueRefusesRatherThanOverflowing)
{
    StreamFixture fixture;
    const std::string path = make_file("q.bin", 1024, 1);
    std::vector<uint8_t> buffer(4096);
    uint32_t accepted = 0;
    for (uint32_t i = 0; i < kMaxRequests + 8u; ++i) {
        if (request(path.c_str(), buffer.data(), 4096, Priority::Normal,
                    nullptr, nullptr) != 0) {
            ++accepted;
        }
    }
    EXPECT_EQ(accepted, kMaxRequests);
    drain(nullptr, nullptr);
    std::remove(path.c_str());
}

TEST(Stream, FirstAccessTraceRecordsOrderAndIgnoresRepeats)
{
    StreamFixture fixture;
    const std::string a = make_file("t1.bin", 512, 1);
    const std::string b = make_file("t2.bin", 512, 2);
    std::vector<uint8_t> buffer(4096);

    request(a.c_str(), buffer.data(), 4096, Priority::Normal, nullptr, nullptr);
    drain(nullptr, nullptr);
    request(b.c_str(), buffer.data(), 4096, Priority::Normal, nullptr, nullptr);
    drain(nullptr, nullptr);
    // Touching 'a' again must NOT move it: the planner wants FIRST access.
    request(a.c_str(), buffer.data(), 4096, Priority::Normal, nullptr, nullptr);
    drain(nullptr, nullptr);

    ASSERT_EQ(trace_count(), 2u);
    EXPECT_STREQ(trace_entry(0), a.c_str());
    EXPECT_STREQ(trace_entry(1), b.c_str());

    char dump[512];
    const uint32_t written = trace_dump(dump, sizeof(dump));
    EXPECT_GT(written, 0u);
    EXPECT_NE(std::string(dump).find(a), std::string::npos);

    std::remove(a.c_str());
    std::remove(b.c_str());
}
