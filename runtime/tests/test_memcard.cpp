// M10 task 3: the PlayerPrefs-equivalent store and its on-card blob format.
// The serialisation is deliberately independent of libmc so it can be tested
// here: a format bug is the kind that quietly eats a player's progress, and
// it must never half-load a corrupt save.
#include "ps2ur/memcard.h"

#include <gtest/gtest.h>

#include <vector>

using namespace ps2ur::memcard;

namespace {

struct PrefsFixture {
    PrefsFixture() { prefs::clear(); }
    ~PrefsFixture() { prefs::clear(); }
};

} // namespace

TEST(Prefs, StoresAndReadsBackEachType)
{
    PrefsFixture fixture;
    prefs::set_int("level", 7);
    prefs::set_float("volume", 0.75f);
    prefs::set_string("name", "ASH");

    EXPECT_EQ(prefs::get_int("level", -1), 7);
    EXPECT_FLOAT_EQ(prefs::get_float("volume", -1.0f), 0.75f);
    EXPECT_STREQ(prefs::get_string("name", "?"), "ASH");
    EXPECT_EQ(prefs::count(), 3u);
}

TEST(Prefs, MissingKeysReturnTheFallback)
{
    PrefsFixture fixture;
    EXPECT_EQ(prefs::get_int("nope", 42), 42);
    EXPECT_FLOAT_EQ(prefs::get_float("nope", 1.5f), 1.5f);
    EXPECT_STREQ(prefs::get_string("nope", "default"), "default");
    EXPECT_FALSE(prefs::has_key("nope"));
}

TEST(Prefs, ReadingWithTheWrongTypeFallsBackRatherThanReinterpreting)
{
    PrefsFixture fixture;
    prefs::set_int("score", 1000);
    // Asking for a float must NOT hand back the int's bit pattern.
    EXPECT_FLOAT_EQ(prefs::get_float("score", -1.0f), -1.0f);
    EXPECT_STREQ(prefs::get_string("score", "?"), "?");
}

TEST(Prefs, SettingTwiceReplacesRatherThanDuplicating)
{
    PrefsFixture fixture;
    prefs::set_int("coins", 1);
    prefs::set_int("coins", 2);
    EXPECT_EQ(prefs::count(), 1u);
    EXPECT_EQ(prefs::get_int("coins", 0), 2);

    // Changing type in place works too.
    prefs::set_string("coins", "many");
    EXPECT_EQ(prefs::count(), 1u);
    EXPECT_STREQ(prefs::get_string("coins", "?"), "many");
}

TEST(Prefs, DeleteRemovesOnlyThatKey)
{
    PrefsFixture fixture;
    prefs::set_int("a", 1);
    prefs::set_int("b", 2);
    prefs::delete_key("a");
    EXPECT_FALSE(prefs::has_key("a"));
    EXPECT_TRUE(prefs::has_key("b"));
    EXPECT_EQ(prefs::count(), 1u);
}

TEST(Prefs, RoundTripsThroughTheBlob)
{
    PrefsFixture fixture;
    prefs::set_int("level", 12);
    prefs::set_float("volume", 0.25f);
    prefs::set_string("player", "Ash");

    uint8_t blob[1024];
    const uint32_t size = prefs::serialize(blob, sizeof(blob));
    ASSERT_GT(size, 12u);

    prefs::clear();
    EXPECT_EQ(prefs::count(), 0u);

    ASSERT_TRUE(prefs::deserialize(blob, size));
    EXPECT_EQ(prefs::get_int("level", -1), 12);
    EXPECT_FLOAT_EQ(prefs::get_float("volume", -1.0f), 0.25f);
    EXPECT_STREQ(prefs::get_string("player", "?"), "Ash");
    EXPECT_EQ(prefs::count(), 3u);
}

TEST(Prefs, SerializeRefusesToTruncate)
{
    PrefsFixture fixture;
    for (int i = 0; i < 20; ++i) {
        char key[8] = {'k', static_cast<char>('a' + i), '\0'};
        prefs::set_string(key, "a reasonably long string value");
    }
    uint8_t tiny[32];
    // Better to write nothing than a store that reads back short.
    EXPECT_EQ(prefs::serialize(tiny, sizeof(tiny)), 0u);
}

TEST(Prefs, DeserializeRejectsCorruptBlobsWithoutTouchingTheStore)
{
    PrefsFixture fixture;
    prefs::set_int("keep", 99);

    uint8_t rubbish[64] = {'N', 'O', 'P', 'E'};
    EXPECT_FALSE(prefs::deserialize(rubbish, sizeof(rubbish)));
    EXPECT_FALSE(prefs::deserialize(nullptr, 64));
    EXPECT_FALSE(prefs::deserialize(rubbish, 4)); // too short for a header

    // A valid header with a truncated body must also be refused.
    uint8_t blob[1024];
    prefs::set_string("name", "something");
    const uint32_t size = prefs::serialize(blob, sizeof(blob));
    ASSERT_GT(size, 20u);
    EXPECT_FALSE(prefs::deserialize(blob, size - 5u));

    // Through all of that the live store is untouched.
    EXPECT_EQ(prefs::get_int("keep", -1), 99);
}

TEST(Prefs, DeserializeRejectsAnImpossibleEntryCount)
{
    PrefsFixture fixture;
    uint8_t blob[16] = {'P', '2', 'P', 'F'};
    blob[4] = 1; // version
    blob[8] = 0xFF;
    blob[9] = 0xFF; // absurd count
    EXPECT_FALSE(prefs::deserialize(blob, sizeof(blob)));
}

TEST(Prefs, EmptyStoreRoundTrips)
{
    PrefsFixture fixture;
    uint8_t blob[64];
    const uint32_t size = prefs::serialize(blob, sizeof(blob));
    EXPECT_EQ(size, 12u); // header only
    EXPECT_TRUE(prefs::deserialize(blob, size));
    EXPECT_EQ(prefs::count(), 0u);
}

TEST(Memcard, StatusTextIsAlwaysUsable)
{
    // These strings end up in front of a player, so none may be empty.
    const Status all[] = {Status::Ok,       Status::NoCard,
                          Status::NotFormatted, Status::Full,
                          Status::WriteProtected, Status::NotFound,
                          Status::Error};
    for (Status status : all) {
        EXPECT_NE(status_text(status), nullptr);
        EXPECT_GT(std::string(status_text(status)).size(), 1u);
    }
}

TEST(Memcard, HostBuildReportsNoCardRatherThanPretending)
{
    // There is no IOP on the workstation. Reporting "no card" keeps callers
    // on the same path a console takes with an empty slot, instead of
    // silently succeeding and losing the save.
    CardInfo info;
    EXPECT_EQ(probe(0, &info), Status::NoCard);
    EXPECT_FALSE(info.present);
}
