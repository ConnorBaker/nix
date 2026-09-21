#include "nix/fetchers/cache.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/store/globals.hh"
#include "nix/util/users.hh"
#include "nix/util/file-system.hh"
#include "nix/util/tests/setting-scopes.hh"

#include <gtest/gtest.h>

#include <limits>

namespace nix::fetchers {

/* The real cache — `CacheImpl` over SQLite — under a cache home of the
   fixture's own. */
class FetcherCacheTest : FreshCacheHome, public ::testing::Test
{
protected:
    void SetUp() override
    {
        nix::initLibStore(/*loadConfig=*/false);
    }
};

TEST_F(FetcherCacheTest, roundTripsAnEmptyNameAndNulBytesThroughTheDatabase)
{
    Settings settings;
    auto cache = settings.getCache();

    Attrs key{{"", std::string("a\0b", 3)}, {"rev", std::string("0123")}};
    Attrs value{
        {"", std::string("\0", 1)},
        {"big", std::numeric_limits<uint64_t>::max()},
        {"flag", Explicit<bool>{false}},
        {"hash", std::string("with\0nul\0", 9)},
        {"zero", uint64_t(0)},
    };

    cache->upsert({"test", key}, value);
    auto found = cache->lookup({"test", key});
    ASSERT_TRUE(found);
    EXPECT_EQ(*found, value);
    EXPECT_EQ(getStrAttr(*found, "hash").size(), 9);

    /* A second write of the same key replaces the row. */
    Attrs value2{{"hash", std::string("other")}};
    cache->upsert({"test", key}, value2);
    found = cache->lookup({"test", key});
    ASSERT_TRUE(found);
    EXPECT_EQ(*found, value2);

    EXPECT_TRUE(pathExists(getCacheDir() / "fetcher-cache-v5.sqlite"));
    EXPECT_FALSE(pathExists(getCacheDir() / "fetcher-cache-v4.sqlite"));
}

TEST_F(FetcherCacheTest, aKeyDifferingOnlyInTypeMisses)
{
    Settings settings;
    auto cache = settings.getCache();

    Attrs asString{{"v", std::string("1")}};
    Attrs asInt{{"v", uint64_t(1)}};
    Attrs asBool{{"v", Explicit<bool>{true}}};

    cache->upsert({"test", asString}, Attrs{{"which", std::string("string")}});
    EXPECT_FALSE(cache->lookup({"test", asInt}));
    EXPECT_FALSE(cache->lookup({"test", asBool}));
    EXPECT_FALSE(cache->lookup({"other", asString}));

    auto found = cache->lookup({"test", asString});
    ASSERT_TRUE(found);
    EXPECT_EQ(getStrAttr(*found, "which"), "string");
}

/* The batch write is a write of every pair in it, whatever order the pairs
   arrive in and whatever order it inserts them in; and a key that appears
   twice in one batch ends at the last value given for it, as two separate
   writes would leave it.  (The insertion is sorted by the encoded key, so
   the order a batch arrives in and the order it is written in differ.) */
TEST_F(FetcherCacheTest, aBatchWritesEveryPairAndKeepsTheLastOfARepeatedKey)
{
    Settings settings;
    auto cache = settings.getCache();

    std::vector<std::pair<Cache::Key, Attrs>> rows;
    /* Keys whose order in the batch is the reverse of their order in the
       database, so that a batch is not written in the order it arrives. */
    for (int i = 9; i >= 0; i--)
        rows.emplace_back(Cache::Key{"batch", {{"k", std::string(1, char('a' + i))}}}, Attrs{{"n", uint64_t(i)}});
    /* A repeat of "e": the last value given for it must stand. */
    rows.emplace_back(Cache::Key{"batch", {{"k", std::string("e")}}}, Attrs{{"n", uint64_t(100)}});

    cache->upsert(rows);

    for (int i = 0; i < 10; i++) {
        auto found = cache->lookup({"batch", {{"k", std::string(1, char('a' + i))}}});
        ASSERT_TRUE(found) << i;
        EXPECT_EQ(getIntAttr(*found, "n"), i == 4 ? 100 : uint64_t(i)) << i;
    }
    EXPECT_FALSE(cache->lookup({"batch", {{"k", std::string("z")}}}));
}

} // namespace nix::fetchers
