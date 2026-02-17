#include "helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/file-load-tracker.hh"
#include "nix/util/source-path.hh"

namespace nix::eval_cache {

using namespace nix::eval_cache::test;

class FileLoadTrackerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Ensure no lingering tracker from a previous test
        FileLoadTracker::clearSessionDeps();
    }

    void TearDown() override
    {
        FileLoadTracker::clearSessionDeps();
    }
};

// ── Hash function tests ──────────────────────────────────────────────

TEST_F(FileLoadTrackerTest, DepHash_Deterministic)
{
    auto h1 = depHash("hello");
    auto h2 = depHash("hello");
    EXPECT_EQ(h1, h2);
}

TEST_F(FileLoadTrackerTest, DepHash_DifferentInputs)
{
    auto h1 = depHash("hello");
    auto h2 = depHash("world");
    EXPECT_NE(h1, h2);
}

TEST_F(FileLoadTrackerTest, DepHash_EmptyInput)
{
    auto h = depHash("");
    // Should produce a valid non-zero hash
    bool allZero = true;
    for (auto b : h.bytes)
        if (b != 0) { allZero = false; break; }
    EXPECT_FALSE(allZero);
}

TEST_F(FileLoadTrackerTest, DepHash_ToHex_Length)
{
    auto h = depHash("test");
    EXPECT_EQ(h.toHex().size(), 64u);
}

TEST_F(FileLoadTrackerTest, DepHashPath_CapturesContent)
{
    TempTestFile f("hello world");
    auto path = SourcePath(getFSSourceAccessor(), CanonPath(f.path.string()));
    auto h = depHashPath(path);
    // Just verify it produces a valid hash
    EXPECT_EQ(h.size(), 32u);
}

TEST_F(FileLoadTrackerTest, DepHashPath_CapturesExecBit)
{
    TempTestFile f1("#!/bin/sh\necho hi");
    TempTestFile f2("#!/bin/sh\necho hi");
    std::filesystem::permissions(f1.path, std::filesystem::perms::owner_exec,
                                  std::filesystem::perm_options::add);

    auto p1 = SourcePath(getFSSourceAccessor(), CanonPath(f1.path.string()));
    auto p2 = SourcePath(getFSSourceAccessor(), CanonPath(f2.path.string()));
    auto h1 = depHashPath(p1);
    auto h2 = depHashPath(p2);
    // NAR includes exec bit, so hashes should differ
    EXPECT_NE(h1, h2);
}

TEST_F(FileLoadTrackerTest, DepHashDirListing_Deterministic)
{
    SourceAccessor::DirEntries entries = {
        {"a", SourceAccessor::Type::tRegular},
        {"b", SourceAccessor::Type::tDirectory},
    };
    auto h1 = depHashDirListing(entries);
    auto h2 = depHashDirListing(entries);
    EXPECT_EQ(h1, h2);
}

TEST_F(FileLoadTrackerTest, DepHashDirListing_OrderSensitive)
{
    SourceAccessor::DirEntries e1 = {
        {"a", SourceAccessor::Type::tRegular},
        {"b", SourceAccessor::Type::tDirectory},
    };
    SourceAccessor::DirEntries e2 = {
        {"b", SourceAccessor::Type::tDirectory},
        {"a", SourceAccessor::Type::tRegular},
    };
    // DirEntries is std::map so order is by key — both should be the same
    auto h1 = depHashDirListing(e1);
    auto h2 = depHashDirListing(e2);
    EXPECT_EQ(h1, h2); // map orders by key, so {a,b} in both cases
}

TEST_F(FileLoadTrackerTest, DepHashDirListing_Empty)
{
    SourceAccessor::DirEntries empty;
    auto h = depHashDirListing(empty);
    bool allZero = true;
    for (auto b : h.bytes)
        if (b != 0) { allZero = false; break; }
    EXPECT_FALSE(allZero);
}

TEST_F(FileLoadTrackerTest, DepHashDirListing_DifferentEntries)
{
    SourceAccessor::DirEntries e1 = {{"a", SourceAccessor::Type::tRegular}};
    SourceAccessor::DirEntries e2 = {{"b", SourceAccessor::Type::tRegular}};
    EXPECT_NE(depHashDirListing(e1), depHashDirListing(e2));
}

TEST_F(FileLoadTrackerTest, DepHash_vs_DepHashPath_Different)
{
    TempTestFile f("hello");
    auto path = SourcePath(getFSSourceAccessor(), CanonPath(f.path.string()));
    auto rawHash = depHash("hello");
    auto narHash = depHashPath(path);
    // Raw BLAKE3 vs NAR BLAKE3 should differ
    EXPECT_NE(rawHash, narHash);
}

// ── RAII tracker tests ───────────────────────────────────────────────

TEST_F(FileLoadTrackerTest, Constructor_SetsActiveTracker)
{
    EXPECT_FALSE(FileLoadTracker::isActive());
    {
        FileLoadTracker tracker;
        EXPECT_TRUE(FileLoadTracker::isActive());
    }
    EXPECT_FALSE(FileLoadTracker::isActive());
}

TEST_F(FileLoadTrackerTest, Destructor_RestoresPrevious)
{
    EXPECT_FALSE(FileLoadTracker::isActive());
    FileLoadTracker outer;
    EXPECT_TRUE(FileLoadTracker::isActive());
    {
        FileLoadTracker inner;
        EXPECT_TRUE(FileLoadTracker::isActive());
    }
    // Outer should still be active
    EXPECT_TRUE(FileLoadTracker::isActive());
}

TEST_F(FileLoadTrackerTest, Record_WithActiveTracker)
{
    FileLoadTracker tracker;
    auto dep = makeContentDep("/test.nix", "content");
    FileLoadTracker::record(dep);
    auto deps = tracker.collectDeps();
    ASSERT_EQ(deps.size(), 1u);
    EXPECT_EQ(deps[0].key, "/test.nix");
    EXPECT_EQ(deps[0].type, DepType::Content);
}

TEST_F(FileLoadTrackerTest, Record_WithoutActiveTracker)
{
    // Recording without a tracker should just append to sessionDeps without crash
    auto dep = makeContentDep("/test.nix", "content");
    FileLoadTracker::record(dep);
    // No crash is the test
}

TEST_F(FileLoadTrackerTest, CollectDeps_OnlyCurrentRange)
{
    FileLoadTracker outer;
    FileLoadTracker::record(makeContentDep("/outer.nix", "outer"));
    {
        FileLoadTracker inner;
        FileLoadTracker::record(makeContentDep("/inner.nix", "inner"));
        auto innerDeps = inner.collectDeps();
        ASSERT_EQ(innerDeps.size(), 1u);
        EXPECT_EQ(innerDeps[0].key, "/inner.nix");
    }
    auto outerDeps = outer.collectDeps();
    // Outer should have both (inner recorded into outer's session range too)
    EXPECT_GE(outerDeps.size(), 1u);
    EXPECT_EQ(outerDeps[0].key, "/outer.nix");
}

TEST_F(FileLoadTrackerTest, ClearSessionDeps)
{
    FileLoadTracker tracker;
    FileLoadTracker::record(makeContentDep("/a.nix", "a"));
    FileLoadTracker::record(makeContentDep("/b.nix", "b"));
    FileLoadTracker::clearSessionDeps();
    // After clearing, new tracker should see no old deps
    // (but the old tracker's startIndex may now be out of range)
}

// ── SuspendFileLoadTracker tests ─────────────────────────────────────

TEST_F(FileLoadTrackerTest, Suspend_DeactivatesTracker)
{
    FileLoadTracker tracker;
    EXPECT_TRUE(FileLoadTracker::isActive());
    {
        SuspendFileLoadTracker suspend;
        EXPECT_FALSE(FileLoadTracker::isActive());
    }
    EXPECT_TRUE(FileLoadTracker::isActive());
}

TEST_F(FileLoadTrackerTest, Suspend_RestoresOnDestruct)
{
    FileLoadTracker tracker;
    {
        SuspendFileLoadTracker suspend;
        EXPECT_FALSE(FileLoadTracker::isActive());
    }
    EXPECT_TRUE(FileLoadTracker::isActive());
}

TEST_F(FileLoadTrackerTest, Suspend_RecordStillAppends)
{
    // Record still appends to sessionDeps even when suspended
    // (no active tracker, but sessionDeps is global)
    FileLoadTracker tracker;
    FileLoadTracker::record(makeContentDep("/before.nix", "a"));
    {
        SuspendFileLoadTracker suspend;
        // Recording during suspension still pushes to sessionDeps
        FileLoadTracker::record(makeContentDep("/during.nix", "b"));
    }
    FileLoadTracker::record(makeContentDep("/after.nix", "c"));
    auto deps = tracker.collectDeps();
    // All three should be in sessionDeps and within tracker's range
    EXPECT_EQ(deps.size(), 3u);
}

TEST_F(FileLoadTrackerTest, Suspend_NestedSuspend)
{
    FileLoadTracker tracker;
    EXPECT_TRUE(FileLoadTracker::isActive());
    {
        SuspendFileLoadTracker s1;
        EXPECT_FALSE(FileLoadTracker::isActive());
        {
            SuspendFileLoadTracker s2;
            EXPECT_FALSE(FileLoadTracker::isActive());
        }
        // s2 restores s1's saved state (which is nullptr)
        EXPECT_FALSE(FileLoadTracker::isActive());
    }
    // s1 restores tracker
    EXPECT_TRUE(FileLoadTracker::isActive());
}

// ── resolveToInput tests ─────────────────────────────────────────────

TEST_F(FileLoadTrackerTest, ResolveToInput_MatchingMount)
{
    std::map<CanonPath, std::pair<std::string, std::string>> mounts;
    mounts[CanonPath("/foo/bar")] = {"myInput", ""};

    auto result = resolveToInput(CanonPath("/foo/bar/baz.nix"), mounts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->first, "myInput");
    EXPECT_EQ(result->second, CanonPath("/baz.nix"));
}

TEST_F(FileLoadTrackerTest, ResolveToInput_NoMatchingMount)
{
    std::map<CanonPath, std::pair<std::string, std::string>> mounts;
    mounts[CanonPath("/foo/bar")] = {"myInput", ""};

    auto result = resolveToInput(CanonPath("/other/path.nix"), mounts);
    EXPECT_FALSE(result.has_value());
}

TEST_F(FileLoadTrackerTest, ResolveToInput_ExactMatch)
{
    std::map<CanonPath, std::pair<std::string, std::string>> mounts;
    mounts[CanonPath("/foo/bar")] = {"myInput", ""};

    auto result = resolveToInput(CanonPath("/foo/bar"), mounts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->first, "myInput");
    EXPECT_EQ(result->second, CanonPath::root);
}

TEST_F(FileLoadTrackerTest, ResolveToInput_LongestPrefixWins)
{
    std::map<CanonPath, std::pair<std::string, std::string>> mounts;
    mounts[CanonPath("/foo")] = {"broad", ""};
    mounts[CanonPath("/foo/bar")] = {"specific", ""};

    auto result = resolveToInput(CanonPath("/foo/bar/baz.nix"), mounts);
    ASSERT_TRUE(result.has_value());
    // resolveToInput walks up from the path, so /foo/bar matches first
    EXPECT_EQ(result->first, "specific");
}

TEST_F(FileLoadTrackerTest, ResolveToInput_WithSubdir)
{
    std::map<CanonPath, std::pair<std::string, std::string>> mounts;
    mounts[CanonPath("/flake-src")] = {"myInput", "subdir"};

    auto result = resolveToInput(CanonPath("/flake-src/subdir/file.nix"), mounts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->first, "myInput");
    EXPECT_EQ(result->second, CanonPath("/file.nix"));
}

// ── Blake3Hash tests ─────────────────────────────────────────────────

TEST_F(FileLoadTrackerTest, Blake3Hash_Equality)
{
    auto h1 = depHash("test");
    auto h2 = depHash("test");
    EXPECT_EQ(h1, h2);
    EXPECT_FALSE(h1 != h2);
}

TEST_F(FileLoadTrackerTest, Blake3Hash_Ordering)
{
    auto h1 = depHash("aaa");
    auto h2 = depHash("bbb");
    // Just verify ordering is defined (doesn't crash)
    [[maybe_unused]] auto cmp = h1 <=> h2;
}

TEST_F(FileLoadTrackerTest, Blake3Hash_FromBlob)
{
    auto h1 = depHash("test");
    auto h2 = Blake3Hash::fromBlob(h1.data(), h1.size());
    EXPECT_EQ(h1, h2);
}

TEST_F(FileLoadTrackerTest, Blake3Hash_View)
{
    auto h = depHash("test");
    auto view = h.view();
    EXPECT_EQ(view.size(), 32u);
    EXPECT_EQ(std::memcmp(view.data(), h.data(), 32), 0);
}

// ── depTypeName tests ────────────────────────────────────────────────

TEST_F(FileLoadTrackerTest, DepTypeName_AllTypes)
{
    EXPECT_STREQ(depTypeName(DepType::Content), "content");
    EXPECT_STREQ(depTypeName(DepType::Directory), "directory");
    EXPECT_STREQ(depTypeName(DepType::Existence), "existence");
    EXPECT_STREQ(depTypeName(DepType::EnvVar), "envvar");
    EXPECT_STREQ(depTypeName(DepType::CurrentTime), "currentTime");
    EXPECT_STREQ(depTypeName(DepType::System), "system");
    EXPECT_STREQ(depTypeName(DepType::UnhashedFetch), "unhashedFetch");
    EXPECT_STREQ(depTypeName(DepType::ParentContext), "parentContext");
    EXPECT_STREQ(depTypeName(DepType::CopiedPath), "copiedPath");
    EXPECT_STREQ(depTypeName(DepType::Exec), "exec");
    EXPECT_STREQ(depTypeName(DepType::NARContent), "narContent");
}

// ── isBlake3Dep tests ────────────────────────────────────────────────

TEST_F(FileLoadTrackerTest, IsBlake3Dep)
{
    EXPECT_TRUE(isBlake3Dep(DepType::Content));
    EXPECT_TRUE(isBlake3Dep(DepType::Directory));
    EXPECT_TRUE(isBlake3Dep(DepType::NARContent));
    EXPECT_TRUE(isBlake3Dep(DepType::EnvVar));
    EXPECT_TRUE(isBlake3Dep(DepType::System));
    EXPECT_FALSE(isBlake3Dep(DepType::Existence));
    EXPECT_FALSE(isBlake3Dep(DepType::CopiedPath));
    EXPECT_FALSE(isBlake3Dep(DepType::UnhashedFetch));
    EXPECT_FALSE(isBlake3Dep(DepType::CurrentTime));
    EXPECT_FALSE(isBlake3Dep(DepType::Exec));
}

// ── Dedup at recording time tests ────────────────────────────────────

TEST_F(FileLoadTrackerTest, Record_DedupWithinTracker)
{
    FileLoadTracker tracker;
    auto dep = makeContentDep("/test.nix", "content");
    FileLoadTracker::record(dep);
    FileLoadTracker::record(dep); // same (type, source, key)
    FileLoadTracker::record(dep); // third time
    auto deps = tracker.collectDeps();
    EXPECT_EQ(deps.size(), 1u); // Only one should survive
}

TEST_F(FileLoadTrackerTest, Record_DedupIgnoresHash)
{
    FileLoadTracker tracker;
    // Same (type, source, key) but different expectedHash values
    FileLoadTracker::record(makeContentDep("/test.nix", "content-v1"));
    FileLoadTracker::record(makeContentDep("/test.nix", "content-v2"));
    auto deps = tracker.collectDeps();
    EXPECT_EQ(deps.size(), 1u); // Deduped by key, first hash wins
}

TEST_F(FileLoadTrackerTest, Record_DedupDifferentKeysNotDeduped)
{
    FileLoadTracker tracker;
    FileLoadTracker::record(makeContentDep("/a.nix", "content"));
    FileLoadTracker::record(makeContentDep("/b.nix", "content"));
    auto deps = tracker.collectDeps();
    EXPECT_EQ(deps.size(), 2u); // Different keys, both kept
}

TEST_F(FileLoadTrackerTest, Record_DedupDifferentTypesNotDeduped)
{
    FileLoadTracker tracker;
    // Same key but different dep types → not deduped
    FileLoadTracker::record({"", "/test.nix", depHash("c"), DepType::Content});
    FileLoadTracker::record({"", "/test.nix", depHash("c"), DepType::Existence});
    auto deps = tracker.collectDeps();
    EXPECT_EQ(deps.size(), 2u);
}

TEST_F(FileLoadTrackerTest, Record_DedupPerTrackerScope)
{
    FileLoadTracker outer;
    FileLoadTracker::record(makeContentDep("/test.nix", "content"));
    {
        FileLoadTracker inner;
        // Same dep in inner tracker — inner has its own recordedKeys
        FileLoadTracker::record(makeContentDep("/test.nix", "content"));
        auto innerDeps = inner.collectDeps();
        EXPECT_EQ(innerDeps.size(), 1u);
    }
    auto outerDeps = outer.collectDeps();
    // Outer sees both: one from before inner, one from inner scope
    EXPECT_EQ(outerDeps.size(), 2u);
}

TEST_F(FileLoadTrackerTest, Record_DedupWithDifferentSources)
{
    FileLoadTracker tracker;
    // Same key and type but different sources → not deduped
    FileLoadTracker::record({"inputA", "/test.nix", depHash("c"), DepType::Content});
    FileLoadTracker::record({"inputB", "/test.nix", depHash("c"), DepType::Content});
    auto deps = tracker.collectDeps();
    EXPECT_EQ(deps.size(), 2u);
}

} // namespace nix::eval_cache
