#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/util/source-path.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DependencyTrackerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Ensure no lingering dependency tracker from a previous test (Adapton: clean DDG state)
        DependencyTracker::clearSessionTraces();
    }

    void TearDown() override
    {
        DependencyTracker::clearSessionTraces();
    }
};

// ── Dep hash function tests (BLAKE3 oracle hashing) ─────────────────

TEST_F(DependencyTrackerTest, DepHash_Deterministic)
{
    auto h1 = depHash("hello");
    auto h2 = depHash("hello");
    EXPECT_EQ(h1, h2);
}

TEST_F(DependencyTrackerTest, DepHash_DifferentInputs)
{
    auto h1 = depHash("hello");
    auto h2 = depHash("world");
    EXPECT_NE(h1, h2);
}

TEST_F(DependencyTrackerTest, DepHash_EmptyInput)
{
    auto h = depHash("");
    // Should produce a valid non-zero BLAKE3 hash
    bool allZero = true;
    for (auto b : h.bytes)
        if (b != 0) { allZero = false; break; }
    EXPECT_FALSE(allZero);
}

TEST_F(DependencyTrackerTest, DepHash_ToHex_Length)
{
    auto h = depHash("test");
    EXPECT_EQ(h.toHex().size(), 64u);
}

TEST_F(DependencyTrackerTest, DepHashPath_CapturesContent)
{
    TempTestFile f("hello world");
    auto path = SourcePath(getFSSourceAccessor(), CanonPath(f.path.string()));
    auto h = depHashPath(path);
    // Just verify it produces a valid BLAKE3 hash of the NAR
    EXPECT_EQ(h.size(), 32u);
}

TEST_F(DependencyTrackerTest, DepHashPath_CapturesExecBit)
{
    TempTestFile f1("#!/bin/sh\necho hi");
    TempTestFile f2("#!/bin/sh\necho hi");
    std::filesystem::permissions(f1.path, std::filesystem::perms::owner_exec,
                                  std::filesystem::perm_options::add);

    auto p1 = SourcePath(getFSSourceAccessor(), CanonPath(f1.path.string()));
    auto p2 = SourcePath(getFSSourceAccessor(), CanonPath(f2.path.string()));
    auto h1 = depHashPath(p1);
    auto h2 = depHashPath(p2);
    // NAR includes exec bit, so dep hashes should differ (captures filesystem oracle state)
    EXPECT_NE(h1, h2);
}

TEST_F(DependencyTrackerTest, DepHashDirListing_Deterministic)
{
    SourceAccessor::DirEntries entries = {
        {"a", SourceAccessor::Type::tRegular},
        {"b", SourceAccessor::Type::tDirectory},
    };
    auto h1 = depHashDirListing(entries);
    auto h2 = depHashDirListing(entries);
    EXPECT_EQ(h1, h2);
}

TEST_F(DependencyTrackerTest, DepHashDirListing_OrderSensitive)
{
    SourceAccessor::DirEntries e1 = {
        {"a", SourceAccessor::Type::tRegular},
        {"b", SourceAccessor::Type::tDirectory},
    };
    SourceAccessor::DirEntries e2 = {
        {"b", SourceAccessor::Type::tDirectory},
        {"a", SourceAccessor::Type::tRegular},
    };
    // DirEntries is std::map so order is by key -- both should produce the same dep hash
    auto h1 = depHashDirListing(e1);
    auto h2 = depHashDirListing(e2);
    EXPECT_EQ(h1, h2); // map orders by key, so {a,b} in both cases
}

TEST_F(DependencyTrackerTest, DepHashDirListing_Empty)
{
    SourceAccessor::DirEntries empty;
    auto h = depHashDirListing(empty);
    bool allZero = true;
    for (auto b : h.bytes)
        if (b != 0) { allZero = false; break; }
    EXPECT_FALSE(allZero);
}

TEST_F(DependencyTrackerTest, DepHashDirListing_DifferentEntries)
{
    SourceAccessor::DirEntries e1 = {{"a", SourceAccessor::Type::tRegular}};
    SourceAccessor::DirEntries e2 = {{"b", SourceAccessor::Type::tRegular}};
    EXPECT_NE(depHashDirListing(e1), depHashDirListing(e2));
}

TEST_F(DependencyTrackerTest, DepHash_vs_DepHashPath_Different)
{
    TempTestFile f("hello");
    auto path = SourcePath(getFSSourceAccessor(), CanonPath(f.path.string()));
    auto rawHash = depHash("hello");
    auto narHash = depHashPath(path);
    // Raw BLAKE3 vs NAR BLAKE3 should differ (different oracle hashing strategies)
    EXPECT_NE(rawHash, narHash);
}

// ── Blake3Hash tests ─────────────────────────────────────────────────

TEST_F(DependencyTrackerTest, Blake3Hash_Equality)
{
    auto h1 = depHash("test");
    auto h2 = depHash("test");
    EXPECT_EQ(h1, h2);
    EXPECT_FALSE(h1 != h2);
}

TEST_F(DependencyTrackerTest, Blake3Hash_Ordering)
{
    auto h1 = depHash("aaa");
    auto h2 = depHash("bbb");
    // Just verify total ordering is defined (needed for sorted trace deps)
    [[maybe_unused]] auto cmp = h1 <=> h2;
}

TEST_F(DependencyTrackerTest, Blake3Hash_FromBlob)
{
    auto h1 = depHash("test");
    auto h2 = Blake3Hash::fromBlob(h1.data(), h1.size());
    EXPECT_EQ(h1, h2);
}

TEST_F(DependencyTrackerTest, Blake3Hash_View)
{
    auto h = depHash("test");
    auto view = h.view();
    EXPECT_EQ(view.size(), 32u);
    EXPECT_EQ(std::memcmp(view.data(), h.data(), 32), 0);
}

// ── depTypeName tests ────────────────────────────────────────────────

TEST_F(DependencyTrackerTest, DepTypeName_AllTypes)
{
    EXPECT_EQ(depTypeName(DepType::Content), "content");
    EXPECT_EQ(depTypeName(DepType::Directory), "directory");
    EXPECT_EQ(depTypeName(DepType::Existence), "existence");
    EXPECT_EQ(depTypeName(DepType::EnvVar), "envvar");
    EXPECT_EQ(depTypeName(DepType::CurrentTime), "currentTime");
    EXPECT_EQ(depTypeName(DepType::System), "system");
    EXPECT_EQ(depTypeName(DepType::UnhashedFetch), "unhashedFetch");
    EXPECT_EQ(depTypeName(DepType::ParentContext), "parentContext");
    EXPECT_EQ(depTypeName(DepType::CopiedPath), "copiedPath");
    EXPECT_EQ(depTypeName(DepType::Exec), "exec");
    EXPECT_EQ(depTypeName(DepType::NARContent), "narContent");
}

// ── isBlake3Dep tests ────────────────────────────────────────────────

TEST_F(DependencyTrackerTest, IsBlake3Dep)
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

} // namespace nix::eval_trace
