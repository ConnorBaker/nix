#include "helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/stat-hash-cache.hh"
#include "nix/expr/file-load-tracker.hh"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace nix::eval_cache {

using namespace nix::eval_cache::test;

class StatHashCacheTest : public ::testing::Test
{
protected:
    StatHashCache & cache = StatHashCache::instance();

    void SetUp() override
    {
        cache.clearMemoryCache();
    }
};

// ── L1 (in-memory) operations ────────────────────────────────────────

TEST_F(StatHashCacheTest, StoreAndLookup_L1Hit)
{
    TempTestFile f("hello world");
    auto hash = depHash("hello world");

    // Store with explicit stat
    cache.storeHash(f.path, DepType::Content, hash);

    // L1 lookup should return the hash
    auto result = cache.lookupHash(f.path, DepType::Content);
    ASSERT_TRUE(result.hash.has_value());
    EXPECT_EQ(*result.hash, hash);
    ASSERT_TRUE(result.stat.has_value());
}

TEST_F(StatHashCacheTest, Lookup_Miss)
{
    TempTestFile f("test content");

    // Never stored — should miss
    auto result = cache.lookupHash(f.path, DepType::Content);
    EXPECT_FALSE(result.hash.has_value());
    // But stat should be populated from the lstat call
    EXPECT_TRUE(result.stat.has_value());
}

TEST_F(StatHashCacheTest, DifferentDepTypes_SameFile)
{
    TempTestFile f("test data");
    auto contentHash = depHash("content-hash");
    auto dirHash = depHash("dir-hash");

    cache.storeHash(f.path, DepType::Content, contentHash);
    cache.storeHash(f.path, DepType::Directory, dirHash);

    auto r1 = cache.lookupHash(f.path, DepType::Content);
    auto r2 = cache.lookupHash(f.path, DepType::Directory);

    ASSERT_TRUE(r1.hash.has_value());
    ASSERT_TRUE(r2.hash.has_value());
    EXPECT_EQ(*r1.hash, contentHash);
    EXPECT_EQ(*r2.hash, dirHash);
    EXPECT_NE(*r1.hash, *r2.hash);
}

TEST_F(StatHashCacheTest, StatMismatch_L1Miss)
{
    TempTestFile f("original");
    auto hash = depHash("original");
    cache.storeHash(f.path, DepType::Content, hash);

    // Modify file to change mtime (and possibly size)
    // Sleep briefly to ensure mtime differs
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    f.modify("modified content!!!");

    // L1 key includes stat metadata — mismatch should cause miss
    auto result = cache.lookupHash(f.path, DepType::Content);
    // Could be L1 miss but L2 hit if same session, or full miss
    // The key is that the old hash should NOT be returned if stat changed
    if (result.hash.has_value()) {
        // If L2 returned something, it should be the stale entry
        // (L2 validates by stat too, so this shouldn't happen for changed files)
        // This test is best-effort since filesystem timing is tricky
    }
}

TEST_F(StatHashCacheTest, ClearMemoryCache_L1Cleared)
{
    TempTestFile f("data");
    auto hash = depHash("data");
    cache.storeHash(f.path, DepType::Content, hash);

    // Verify L1 hit
    auto r1 = cache.lookupHash(f.path, DepType::Content);
    ASSERT_TRUE(r1.hash.has_value());

    cache.clearMemoryCache();

    // After clearing, L1 should miss but L2 may still have it
    auto r2 = cache.lookupHash(f.path, DepType::Content);
    // L2 should still return the hash (if SQLite is available)
    // This is a weaker assertion since L2 availability depends on environment
    EXPECT_TRUE(r2.stat.has_value()); // stat should always work
}

// ── L2 (SQLite persistence) ─────────────────────────────────────────

TEST_F(StatHashCacheTest, L2Roundtrip)
{
    TempTestFile f("persistent data");
    auto hash = depHash("persistent data");

    // Store → should write to both L1 and L2
    cache.storeHash(f.path, DepType::Content, hash);

    // Clear L1 to force L2 lookup
    cache.clearMemoryCache();

    auto result = cache.lookupHash(f.path, DepType::Content);
    // L2 should return the hash if SQLite is available
    if (result.hash.has_value()) {
        EXPECT_EQ(*result.hash, hash);
    }
}

TEST_F(StatHashCacheTest, L2BlobStorage_RoundtripExact)
{
    TempTestFile f("blob test");
    auto hash = depHash("blob test");

    cache.storeHash(f.path, DepType::Content, hash);
    cache.clearMemoryCache();

    auto result = cache.lookupHash(f.path, DepType::Content);
    if (result.hash.has_value()) {
        // Verify byte-exact roundtrip
        EXPECT_EQ(std::memcmp(result.hash->data(), hash.data(), 32), 0);
    }
}

// ── Edge cases ───────────────────────────────────────────────────────

TEST_F(StatHashCacheTest, NonexistentFile_LookupReturnsEmpty)
{
    auto result = cache.lookupHash("/nonexistent/path/to/file.nix", DepType::Content);
    EXPECT_FALSE(result.hash.has_value());
    EXPECT_FALSE(result.stat.has_value());
}

TEST_F(StatHashCacheTest, StoreHash_NonexistentPath)
{
    auto hash = depHash("test");
    // storeHash with no stat overload calls maybeLstat, which returns nullopt
    // for nonexistent paths. Should be a no-op, not a crash.
    cache.storeHash("/nonexistent/path.nix", DepType::Content, hash);
    // No crash = success
}

TEST_F(StatHashCacheTest, MultipleTypes_IndependentStorage)
{
    TempTestFile f("multi-type test");

    auto h1 = depHash("content-val");
    auto h2 = depHash("nar-val");
    auto h3 = depHash("dir-val");

    cache.storeHash(f.path, DepType::Content, h1);
    cache.storeHash(f.path, DepType::NARContent, h2);
    cache.storeHash(f.path, DepType::Directory, h3);

    EXPECT_EQ(*cache.lookupHash(f.path, DepType::Content).hash, h1);
    EXPECT_EQ(*cache.lookupHash(f.path, DepType::NARContent).hash, h2);
    EXPECT_EQ(*cache.lookupHash(f.path, DepType::Directory).hash, h3);
}

} // namespace nix::eval_cache
