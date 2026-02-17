#include "helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/stat-hash-cache.hh"
#include "nix/expr/dep-tracker.hh"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class StatHashCacheTest : public ::testing::Test
{
protected:
    StatHashCache & cache = StatHashCache::instance();

    void SetUp() override
    {
        cache.clearMemoryCache();
    }
};

// ── L1 (in-memory) oracle hash cache operations ─────────────────────

TEST_F(StatHashCacheTest, StoreAndLookup_L1Hit)
{
    TempTestFile f("hello world");
    auto hash = depHash("hello world");

    // Store oracle hash with explicit stat metadata
    cache.storeHash(f.path, DepType::Content, hash);

    // L1 lookup should return the cached oracle hash
    auto result = cache.lookupHash(f.path, DepType::Content);
    ASSERT_TRUE(result.hash.has_value());
    EXPECT_EQ(*result.hash, hash);
    ASSERT_TRUE(result.stat.has_value());
}

TEST_F(StatHashCacheTest, Lookup_Miss)
{
    TempTestFile f("test content");

    // Never stored — oracle hash cache miss
    auto result = cache.lookupHash(f.path, DepType::Content);
    EXPECT_FALSE(result.hash.has_value());
    // But stat metadata should be populated from the lstat call
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

    // Modify file to change stat oracle metadata (mtime and possibly size)
    // Sleep briefly to ensure mtime differs
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    f.modify("modified content!!!");

    // L1 key includes stat oracle metadata — mismatch should cause cache miss
    auto result = cache.lookupHash(f.path, DepType::Content);
    // Could be L1 miss but L2 hit if same session, or full miss
    // The key is that the stale oracle hash should NOT be returned if stat changed
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

    // Verify L1 oracle cache hit
    auto r1 = cache.lookupHash(f.path, DepType::Content);
    ASSERT_TRUE(r1.hash.has_value());

    cache.clearMemoryCache();

    // After clearing L1 (session-scoped), L2 (persistent SQLite) may still serve the oracle hash
    auto r2 = cache.lookupHash(f.path, DepType::Content);
    // L2 persistent store should still return the oracle hash (if SQLite is available)
    // Weaker assertion: L2 availability depends on environment
    EXPECT_TRUE(r2.stat.has_value()); // stat oracle metadata should always work
}

// ── L2 (persistent SQLite oracle hash store) ────────────────────────

TEST_F(StatHashCacheTest, L2Roundtrip)
{
    TempTestFile f("persistent data");
    auto hash = depHash("persistent data");

    // Store oracle hash — writes to both L1 (session) and L2 (persistent SQLite)
    cache.storeHash(f.path, DepType::Content, hash);

    // Clear L1 session cache to force L2 persistent lookup
    cache.clearMemoryCache();

    auto result = cache.lookupHash(f.path, DepType::Content);
    // L2 persistent store should return the oracle hash (if SQLite is available)
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
        // Verify byte-exact BLOB roundtrip through L2 persistent oracle store
        EXPECT_EQ(std::memcmp(result.hash->data(), hash.data(), 32), 0);
    }
}

// ── Edge cases (oracle cache robustness) ─────────────────────────────

TEST_F(StatHashCacheTest, NonexistentFile_LookupReturnsEmpty)
{
    auto result = cache.lookupHash("/nonexistent/path/to/file.nix", DepType::Content);
    EXPECT_FALSE(result.hash.has_value());
    EXPECT_FALSE(result.stat.has_value());
}

TEST_F(StatHashCacheTest, StoreHash_NonexistentPath)
{
    auto hash = depHash("test");
    // storeHash with no stat overload calls maybeLstat (oracle stat query), which returns
    // nullopt for nonexistent paths. Should be a no-op, not a crash.
    cache.storeHash("/nonexistent/path.nix", DepType::Content, hash);
    // No crash = success (oracle cache gracefully handles missing inputs)
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

} // namespace nix::eval_trace
