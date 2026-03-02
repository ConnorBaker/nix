#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/util/source-path.hh"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class StatCachedHashTest : public ::testing::Test
{
protected:
    ScopedCacheDir cacheDir;
};

// ── depHashFile: stat-cached Content oracle hash ─────────────────────

TEST_F(StatCachedHashTest, DepHashFile_Deterministic)
{
    TempTestFile f("hello world");
    auto path = SourcePath(getFSSourceAccessor(), CanonPath(f.path.string()));

    auto h1 = depHashFile(path);
    auto h2 = depHashFile(path);
    EXPECT_EQ(h1, h2);
}

TEST_F(StatCachedHashTest, DepHashFile_MatchesRawHash)
{
    TempTestFile f("hello world");
    auto path = SourcePath(getFSSourceAccessor(), CanonPath(f.path.string()));

    auto cached = depHashFile(path);
    auto raw = depHash("hello world");
    EXPECT_EQ(cached, raw);
}

TEST_F(StatCachedHashTest, DepHashFile_CachedOnSecondCall)
{
    TempTestFile f("cached content");
    auto path = SourcePath(getFSSourceAccessor(), CanonPath(f.path.string()));

    // First call computes and caches
    auto h1 = depHashFile(path);
    // Second call should use stat-cache (same result)
    auto h2 = depHashFile(path);
    EXPECT_EQ(h1, h2);
}

TEST_F(StatCachedHashTest, DepHashFile_DetectsModification)
{
    TempTestFile f("original");
    auto path = SourcePath(getFSSourceAccessor(), CanonPath(f.path.string()));

    auto h1 = depHashFile(path);

    // Modify with different-sized content to ensure stat metadata changes
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    f.modify("modified content!!!");
    getFSSourceAccessor()->invalidateCache(CanonPath(f.path.string()));

    auto h2 = depHashFile(path);
    EXPECT_NE(h1, h2);
    EXPECT_EQ(h2, depHash("modified content!!!"));
}

TEST_F(StatCachedHashTest, DepHashFile_DifferentFiles)
{
    TempTestFile f1("file-one");
    TempTestFile f2("file-two");
    auto p1 = SourcePath(getFSSourceAccessor(), CanonPath(f1.path.string()));
    auto p2 = SourcePath(getFSSourceAccessor(), CanonPath(f2.path.string()));

    EXPECT_NE(depHashFile(p1), depHashFile(p2));
}

// ── depHashPathCached: stat-cached NARContent oracle hash ────────────

TEST_F(StatCachedHashTest, DepHashPathCached_Deterministic)
{
    TempTestFile f("nar test");
    auto path = SourcePath(getFSSourceAccessor(), CanonPath(f.path.string()));

    auto h1 = depHashPathCached(path);
    auto h2 = depHashPathCached(path);
    EXPECT_EQ(h1, h2);
}

TEST_F(StatCachedHashTest, DepHashPathCached_MatchesUncached)
{
    TempTestFile f("nar content");
    auto path = SourcePath(getFSSourceAccessor(), CanonPath(f.path.string()));

    auto cached = depHashPathCached(path);
    auto uncached = depHashPath(path);
    EXPECT_EQ(cached, uncached);
}

TEST_F(StatCachedHashTest, DepHashPathCached_DiffersFromContentHash)
{
    TempTestFile f("same content");
    auto path = SourcePath(getFSSourceAccessor(), CanonPath(f.path.string()));

    // NARContent hash includes NAR header/metadata, so it differs from raw Content hash
    auto narHash = depHashPathCached(path);
    auto contentHash = depHashFile(path);
    EXPECT_NE(narHash, contentHash);
}

// ── depHashDirListingCached: stat-cached Directory oracle hash ───────

TEST_F(StatCachedHashTest, DepHashDirListingCached_Deterministic)
{
    auto dir = std::filesystem::temp_directory_path()
             / ("nix-test-dir-cache-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "a.txt") << "a";
    std::ofstream(dir / "b.txt") << "b";

    auto path = SourcePath(getFSSourceAccessor(), CanonPath(dir.string()));
    auto entries = path.readDirectory();

    auto h1 = depHashDirListingCached(path, entries);
    auto h2 = depHashDirListingCached(path, entries);
    EXPECT_EQ(h1, h2);

    std::filesystem::remove_all(dir);
}

TEST_F(StatCachedHashTest, DepHashDirListingCached_MatchesUncached)
{
    auto dir = std::filesystem::temp_directory_path()
             / ("nix-test-dir-cache2-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "x.nix") << "42";

    auto path = SourcePath(getFSSourceAccessor(), CanonPath(dir.string()));
    auto entries = path.readDirectory();

    auto cached = depHashDirListingCached(path, entries);
    auto uncached = depHashDirListing(entries);
    EXPECT_EQ(cached, uncached);

    std::filesystem::remove_all(dir);
}

} // namespace nix::eval_trace
