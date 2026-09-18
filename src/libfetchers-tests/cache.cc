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

} // namespace nix::fetchers
