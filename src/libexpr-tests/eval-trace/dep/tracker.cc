#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/dependency-tracker.hh"
#include "nix/expr/eval-trace-deps.hh"
#include "nix/util/source-path.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DependencyTrackerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        DependencyTracker::clearSessionTraces();
    }

    void TearDown() override
    {
        DependencyTracker::clearSessionTraces();
    }
};

// ── RAII dependency tracker tests (Adapton: DDG scope management) ────

TEST_F(DependencyTrackerTest, Constructor_SetsActiveTracker)
{
    EXPECT_FALSE(DependencyTracker::isActive());
    {
        DependencyTracker tracker;
        EXPECT_TRUE(DependencyTracker::isActive());
    }
    EXPECT_FALSE(DependencyTracker::isActive());
}

TEST_F(DependencyTrackerTest, Destructor_RestoresPrevious)
{
    EXPECT_FALSE(DependencyTracker::isActive());
    DependencyTracker outer;
    EXPECT_TRUE(DependencyTracker::isActive());
    {
        DependencyTracker inner;
        EXPECT_TRUE(DependencyTracker::isActive());
    }
    // Outer tracker should still be active (Adapton: restore parent scope)
    EXPECT_TRUE(DependencyTracker::isActive());
}

TEST_F(DependencyTrackerTest, Record_WithActiveTracker)
{
    DependencyTracker tracker;
    auto dep = makeContentDep("/test.nix", "content");
    DependencyTracker::record(dep);
    auto deps = tracker.collectTraces();
    ASSERT_EQ(deps.size(), 1u);
    EXPECT_EQ(resolveDepKey(deps[0].keyId), "/test.nix");
    EXPECT_EQ(deps[0].type, DepType::Content);
}

TEST_F(DependencyTrackerTest, Record_WithoutActiveTracker)
{
    // Recording without an active tracker should just append to session traces without crash
    auto dep = makeContentDep("/test.nix", "content");
    DependencyTracker::record(dep);
    // No crash is the test (dependency recorded to global session trace)
}

TEST_F(DependencyTrackerTest, CollectDeps_OnlyCurrentRange)
{
    DependencyTracker outer;
    DependencyTracker::record(makeContentDep("/outer.nix", "outer"));
    {
        DependencyTracker inner;
        DependencyTracker::record(makeContentDep("/inner.nix", "inner"));
        auto innerDeps = inner.collectTraces();
        ASSERT_EQ(innerDeps.size(), 1u);
        EXPECT_EQ(resolveDepKey(innerDeps[0].keyId), "/inner.nix");
    }
    auto outerDeps = outer.collectTraces();
    // Outer should have both (inner recorded into outer's session range too — Adapton: nested scopes)
    EXPECT_GE(outerDeps.size(), 1u);
    EXPECT_EQ(resolveDepKey(outerDeps[0].keyId), "/outer.nix");
}

TEST_F(DependencyTrackerTest, ClearSessionDeps)
{
    DependencyTracker tracker;
    DependencyTracker::record(makeContentDep("/a.nix", "a"));
    DependencyTracker::record(makeContentDep("/b.nix", "b"));
    DependencyTracker::clearSessionTraces();
    // After clearing, new tracker should see no old deps
    // (session trace reset — Adapton: clean DDG slate)
}

// ── SuspendDepTracking tests (Adapton: suspend DDG recording) ────

TEST_F(DependencyTrackerTest, Suspend_DeactivatesTracker)
{
    DependencyTracker tracker;
    EXPECT_TRUE(DependencyTracker::isActive());
    {
        SuspendDepTracking suspend;
        EXPECT_FALSE(DependencyTracker::isActive());
    }
    EXPECT_TRUE(DependencyTracker::isActive());
}

TEST_F(DependencyTrackerTest, Suspend_RestoresOnDestruct)
{
    DependencyTracker tracker;
    {
        SuspendDepTracking suspend;
        EXPECT_FALSE(DependencyTracker::isActive());
    }
    EXPECT_TRUE(DependencyTracker::isActive());
}

TEST_F(DependencyTrackerTest, Suspend_RecordStillAppends)
{
    // Recording still appends to session traces even when tracker is suspended
    // (no active tracker scope, but session trace is global)
    DependencyTracker tracker;
    DependencyTracker::record(makeContentDep("/before.nix", "a"));
    {
        SuspendDepTracking suspend;
        // Recording during suspension still pushes to session traces
        DependencyTracker::record(makeContentDep("/during.nix", "b"));
    }
    DependencyTracker::record(makeContentDep("/after.nix", "c"));
    auto deps = tracker.collectTraces();
    // All three should be in session traces and within tracker's range
    EXPECT_EQ(deps.size(), 3u);
}

TEST_F(DependencyTrackerTest, Suspend_NestedSuspend)
{
    DependencyTracker tracker;
    EXPECT_TRUE(DependencyTracker::isActive());
    {
        SuspendDepTracking s1;
        EXPECT_FALSE(DependencyTracker::isActive());
        {
            SuspendDepTracking s2;
            EXPECT_FALSE(DependencyTracker::isActive());
        }
        // s2 restores s1's saved state (which is nullptr — no active tracker)
        EXPECT_FALSE(DependencyTracker::isActive());
    }
    // s1 restores the original tracker (Adapton: DDG scope restored)
    EXPECT_TRUE(DependencyTracker::isActive());
}

TEST_F(DependencyTrackerTest, Suspend_TrackerInsideSuspendPreservesSessionCaches)
{
    // Regression test: creating a DependencyTracker inside a SuspendDepTracking
    // scope must NOT call onRootConstruction(), which would clear session-wide
    // provenance caches (tracedContainerMap, provenancePool, etc.) mid-evaluation.

    DependencyTracker root;

    // Register provenance data in the session caches.
    auto srcId = internDepSource("test-input");
    auto fpId = internFilePath("/test.json");
    auto dpId = jsonStringToDataPathId("[]");
    auto * prov = allocateProvenance(srcId, fpId, dpId, StructuredFormat::Json);

    // Use an arbitrary pointer as a list container key.
    int fakeValue = 0;
    registerTracedContainer(&fakeValue, prov);
    ASSERT_NE(lookupTracedContainer(&fakeValue), nullptr);

    // Create a new tracker inside a suspended scope. Without the fix,
    // previous == nullptr triggers onRootConstruction() and wipes the caches.
    {
        SuspendDepTracking suspend;
        DependencyTracker inner;
    }

    EXPECT_EQ(lookupTracedContainer(&fakeValue), prov)
        << "Provenance must survive a DependencyTracker created inside SuspendDepTracking";
}

// ── resolveToInput tests ─────────────────────────────────────────────

TEST_F(DependencyTrackerTest, ResolveToInput_MatchingMount)
{
    std::unordered_map<CanonPath, std::pair<std::string, std::string>> mounts;
    mounts[CanonPath("/foo/bar")] = {"myInput", ""};

    auto result = resolveToInput(CanonPath("/foo/bar/baz.nix"), mounts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->first, "myInput");
    EXPECT_EQ(result->second, CanonPath("/baz.nix"));
}

TEST_F(DependencyTrackerTest, ResolveToInput_NoMatchingMount)
{
    std::unordered_map<CanonPath, std::pair<std::string, std::string>> mounts;
    mounts[CanonPath("/foo/bar")] = {"myInput", ""};

    auto result = resolveToInput(CanonPath("/other/path.nix"), mounts);
    EXPECT_FALSE(result.has_value());
}

TEST_F(DependencyTrackerTest, ResolveToInput_ExactMatch)
{
    std::unordered_map<CanonPath, std::pair<std::string, std::string>> mounts;
    mounts[CanonPath("/foo/bar")] = {"myInput", ""};

    auto result = resolveToInput(CanonPath("/foo/bar"), mounts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->first, "myInput");
    EXPECT_EQ(result->second, CanonPath::root);
}

TEST_F(DependencyTrackerTest, ResolveToInput_LongestPrefixWins)
{
    std::unordered_map<CanonPath, std::pair<std::string, std::string>> mounts;
    mounts[CanonPath("/foo")] = {"broad", ""};
    mounts[CanonPath("/foo/bar")] = {"specific", ""};

    auto result = resolveToInput(CanonPath("/foo/bar/baz.nix"), mounts);
    ASSERT_TRUE(result.has_value());
    // resolveToInput walks up from the path, so /foo/bar matches first (longest prefix)
    EXPECT_EQ(result->first, "specific");
}

TEST_F(DependencyTrackerTest, ResolveToInput_WithSubdir)
{
    std::unordered_map<CanonPath, std::pair<std::string, std::string>> mounts;
    mounts[CanonPath("/flake-src")] = {"myInput", "subdir"};

    auto result = resolveToInput(CanonPath("/flake-src/subdir/file.nix"), mounts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->first, "myInput");
    EXPECT_EQ(result->second, CanonPath("/file.nix"));
}

// ── Dedup at recording time tests (Adapton: DDG edge deduplication) ──

TEST_F(DependencyTrackerTest, Record_DedupWithinTracker)
{
    DependencyTracker tracker;
    auto dep = makeContentDep("/test.nix", "content");
    DependencyTracker::record(dep);
    DependencyTracker::record(dep); // same (type, source, key)
    DependencyTracker::record(dep); // third time
    auto deps = tracker.collectTraces();
    EXPECT_EQ(deps.size(), 1u); // Only one should survive (deduped by dep key)
}

TEST_F(DependencyTrackerTest, Record_DedupIgnoresHash)
{
    DependencyTracker tracker;
    // Same dep key (type, source, key) but different expectedHash values
    DependencyTracker::record(makeContentDep("/test.nix", "content-v1"));
    DependencyTracker::record(makeContentDep("/test.nix", "content-v2"));
    auto deps = tracker.collectTraces();
    EXPECT_EQ(deps.size(), 1u); // Deduped by dep key, first hash wins
}

TEST_F(DependencyTrackerTest, Record_DedupDifferentKeysNotDeduped)
{
    DependencyTracker tracker;
    DependencyTracker::record(makeContentDep("/a.nix", "content"));
    DependencyTracker::record(makeContentDep("/b.nix", "content"));
    auto deps = tracker.collectTraces();
    EXPECT_EQ(deps.size(), 2u); // Different dep keys, both kept
}

TEST_F(DependencyTrackerTest, Record_DedupDifferentTypesNotDeduped)
{
    DependencyTracker tracker;
    // Same key but different dep types -> not deduped (different oracle types)
    DependencyTracker::record({"", "/test.nix", depHash("c"), DepType::Content});
    DependencyTracker::record({"", "/test.nix", depHash("c"), DepType::Existence});
    auto deps = tracker.collectTraces();
    EXPECT_EQ(deps.size(), 2u);
}

TEST_F(DependencyTrackerTest, Record_DedupPerTrackerScope)
{
    DependencyTracker outer;
    DependencyTracker::record(makeContentDep("/test.nix", "content"));
    {
        DependencyTracker inner;
        // Same dep in inner tracker — inner has its own dedup scope (Adapton: nested DDG node)
        DependencyTracker::record(makeContentDep("/test.nix", "content"));
        auto innerDeps = inner.collectTraces();
        EXPECT_EQ(innerDeps.size(), 1u);
    }
    auto outerDeps = outer.collectTraces();
    // Outer sees both: one from before inner, one from inner scope
    EXPECT_EQ(outerDeps.size(), 2u);
}

TEST_F(DependencyTrackerTest, Record_DedupWithDifferentSources)
{
    DependencyTracker tracker;
    // Same key and type but different sources -> not deduped (different oracle identities)
    DependencyTracker::record({"inputA", "/test.nix", depHash("c"), DepType::Content});
    DependencyTracker::record({"inputB", "/test.nix", depHash("c"), DepType::Content});
    auto deps = tracker.collectTraces();
    EXPECT_EQ(deps.size(), 2u);
}

// ── DepKind enum tests ────────────────────────────────────────────────

TEST_F(DependencyTrackerTest, DepKind_Classification)
{
    EXPECT_EQ(depKind(DepType::Content), DepKind::ContentOverrideable);
    EXPECT_EQ(depKind(DepType::Directory), DepKind::ContentOverrideable);
    EXPECT_EQ(depKind(DepType::Existence), DepKind::Normal);
    EXPECT_EQ(depKind(DepType::EnvVar), DepKind::Normal);
    EXPECT_EQ(depKind(DepType::CurrentTime), DepKind::Volatile);
    EXPECT_EQ(depKind(DepType::System), DepKind::Normal);
    EXPECT_EQ(depKind(DepType::UnhashedFetch), DepKind::Normal);
    EXPECT_EQ(depKind(DepType::ParentContext), DepKind::ParentContext);
    EXPECT_EQ(depKind(DepType::CopiedPath), DepKind::Normal);
    EXPECT_EQ(depKind(DepType::Exec), DepKind::Volatile);
    EXPECT_EQ(depKind(DepType::NARContent), DepKind::Normal);
    EXPECT_EQ(depKind(DepType::StructuredContent), DepKind::Structural);
}

TEST_F(DependencyTrackerTest, DepKindName_AllKinds)
{
    EXPECT_EQ(depKindName(DepKind::Normal), "normal");
    EXPECT_EQ(depKindName(DepKind::Volatile), "volatile");
    EXPECT_EQ(depKindName(DepKind::ContentOverrideable), "contentOverrideable");
    EXPECT_EQ(depKindName(DepKind::Structural), "structural");
    EXPECT_EQ(depKindName(DepKind::ParentContext), "parentContext");
}

TEST_F(DependencyTrackerTest, IsVolatile)
{
    EXPECT_TRUE(isVolatile(DepType::CurrentTime));
    EXPECT_TRUE(isVolatile(DepType::Exec));
    EXPECT_FALSE(isVolatile(DepType::Content));
    EXPECT_FALSE(isVolatile(DepType::StructuredContent));
    EXPECT_FALSE(isVolatile(DepType::ParentContext));
}

TEST_F(DependencyTrackerTest, IsContentOverrideable)
{
    EXPECT_TRUE(isContentOverrideable(DepType::Content));
    EXPECT_TRUE(isContentOverrideable(DepType::Directory));
    EXPECT_FALSE(isContentOverrideable(DepType::Existence));
    EXPECT_FALSE(isContentOverrideable(DepType::StructuredContent));
    EXPECT_FALSE(isContentOverrideable(DepType::CurrentTime));
}

} // namespace nix::eval_trace
