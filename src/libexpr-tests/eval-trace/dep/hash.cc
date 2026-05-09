#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/util/source-path.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;


// ── Dep hash function tests (active-backend oracle hashing) ──────────

TEST(DepHashBasicTest, Hash_DepHash_Deterministic)
{
    auto h1 = depHash("hello");
    auto h2 = depHash("hello");
    EXPECT_EQ(h1, h2);
}

TEST(DepHashBasicTest, Hash_DepHash_DifferentInputsDiffer)
{
    auto h1 = depHash("hello");
    auto h2 = depHash("world");
    EXPECT_NE(h1, h2);
}

TEST(DepHashBasicTest, Hash_DepHash_EmptyInputNonZero)
{
    auto h = depHash("");
    // Should produce a valid non-zero eval-trace digest.
    bool allZero = true;
    for (auto b : h.value.bytes)
        if (b != 0) { allZero = false; break; }
    EXPECT_FALSE(allZero);
}

TEST(DepHashBasicTest, DepHash_ToHex_Length)
{
    auto h = depHash("test");
    EXPECT_EQ(h.value.toHex().size(), 64u);
}

TEST(DepHashBasicTest, Hash_DepHashPath_CapturesContent)
{
    TempTestFile f("hello world");
    auto path = SourcePath(getFSSourceAccessor(), CanonPath(f.path.string()));
    auto h = depHashPath(path);
    // Just verify it produces a valid eval-trace digest of the NAR.
    EXPECT_EQ(h.value.size(), 32u);
}

TEST(DepHashBasicTest, Hash_DepHashPath_CapturesExecBit)
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

TEST(DepHashBasicTest, Hash_DirListing_Deterministic)
{
    SourceAccessor::DirEntries entries = {
        {"a", SourceAccessor::Type::tRegular},
        {"b", SourceAccessor::Type::tDirectory},
    };
    auto h1 = depHashDirListing(entries);
    auto h2 = depHashDirListing(entries);
    EXPECT_EQ(h1, h2);
}

TEST(DepHashBasicTest, Hash_DirListing_OrderIndependent)
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

TEST(DepHashBasicTest, Hash_DirListing_EmptyNonZero)
{
    SourceAccessor::DirEntries empty;
    auto h = depHashDirListing(empty);
    bool allZero = true;
    for (auto b : h.value.bytes)
        if (b != 0) { allZero = false; break; }
    EXPECT_FALSE(allZero);
}

TEST(DepHashBasicTest, Hash_DirListing_DifferentEntriesDiffer)
{
    SourceAccessor::DirEntries e1 = {{"a", SourceAccessor::Type::tRegular}};
    SourceAccessor::DirEntries e2 = {{"b", SourceAccessor::Type::tRegular}};
    EXPECT_NE(depHashDirListing(e1), depHashDirListing(e2));
}

TEST(DepHashBasicTest, DepHash_vs_DepHashPath_Different)
{
    TempTestFile f("hello");
    auto path = SourcePath(getFSSourceAccessor(), CanonPath(f.path.string()));
    auto rawHash = depHash("hello");
    auto narHash = depHashPath(path);
    // Raw byte digest vs NAR digest should differ (different oracle hashing strategies).
    EXPECT_NE(rawHash, narHash);
}

// ── EvalTraceHash tests ─────────────────────────────────────────────────

TEST(DepHashBasicTest, Hash_EvalTraceHash_Equality)
{
    auto h1 = depHash("test");
    auto h2 = depHash("test");
    EXPECT_EQ(h1, h2);
    EXPECT_FALSE(h1 != h2);
}

TEST(DepHashBasicTest, Hash_EvalTraceHash_OrderingDefined)
{
    auto h1 = depHash("aaa");
    auto h2 = depHash("bbb");
    // Just verify total ordering is defined (needed for sorted trace deps)
    [[maybe_unused]] auto cmp = h1 <=> h2;
}

TEST(DepHashBasicTest, Hash_EvalTraceHash_FromBlobRoundTrips)
{
    auto h1 = depHash("test");
    auto h2 = EvalTraceHash::fromBlob(h1.value.data(), h1.value.size());
    EXPECT_EQ(h1.value, h2);
}

TEST(DepHashBasicTest, Hash_EvalTraceHash_ViewMatchesData)
{
    auto h = depHash("test");
    auto view = h.value.view();
    EXPECT_EQ(view.size(), 32u);
    EXPECT_EQ(std::memcmp(view.data(), h.value.data(), 32), 0);
}

// ── queryKindName tests ────────────────────────────────────────────────

TEST(DepHashBasicTest, Hash_QueryKindName_AllTypesNamed)
{
    EXPECT_EQ(queryKindName(CanonicalQueryKind::FileBytes), "fileBytes");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::DirectoryEntries), "directoryEntries");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::ExistenceCheck), "existenceCheck");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::EnvironmentLookup), "environmentLookup");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::VolatileTime), "volatileTime");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::SessionSystemValue), "sessionSystemValue");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::RuntimeFetchIdentity), "runtimeFetchIdentity");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::TraceValueContext), "traceValueContext");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::TraceParentSlot), "traceParentSlot");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::DerivedStorePath), "derivedStorePath");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::VolatileExec), "volatileExec");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::NarIdentity), "narIdentity");
}

// ── isDigestDep tests ────────────────────────────────────────────────

TEST(DepHashBasicTest, Hash_IsDigestDep_CorrectKindsIdentified)
{
    EXPECT_TRUE(isDigestDep(CanonicalQueryKind::FileBytes));
    EXPECT_TRUE(isDigestDep(CanonicalQueryKind::DirectoryEntries));
    EXPECT_TRUE(isDigestDep(CanonicalQueryKind::NarIdentity));
    EXPECT_TRUE(isDigestDep(CanonicalQueryKind::EnvironmentLookup));
    EXPECT_TRUE(isDigestDep(CanonicalQueryKind::SessionSystemValue));
    EXPECT_FALSE(isDigestDep(CanonicalQueryKind::ExistenceCheck));
    EXPECT_FALSE(isDigestDep(CanonicalQueryKind::DerivedStorePath));
    EXPECT_FALSE(isDigestDep(CanonicalQueryKind::RuntimeFetchIdentity));
    EXPECT_FALSE(isDigestDep(CanonicalQueryKind::VolatileTime));
    EXPECT_FALSE(isDigestDep(CanonicalQueryKind::VolatileExec));
}

} // namespace nix::eval_trace
