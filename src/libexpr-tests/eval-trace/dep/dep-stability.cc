#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-trace/result.hh"
#include "nix/expr/value.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

/// Extract dep keys from a dep vector for exact-match assertions.
static std::vector<std::string> keys(InterningPools & pools, const std::vector<Dep> & deps)
{
    std::vector<std::string> out;
    out.reserve(deps.size());
    for (auto & d : deps)
        out.push_back(std::string(pools.resolve(d.key.keyId)));
    return out;
}

class DepStabilityTest : public ::testing::Test
{
protected:
    InterningPools pools;
    void SetUp() override { DependencyTracker::clearSessionTraces(); }
    void TearDown() override { DependencyTracker::clearSessionTraces(); }

    /// Simulate a parent evaluation that records its own deps, then a child
    /// evaluates fresh (recording into sessionTraces). Returns the parent's
    /// collected deps after excluding the child's range.
    std::vector<std::string> runWithFreshChild(
        const std::vector<Dep> & parentDeps,
        const std::vector<Dep> & childDeps)
    {
        DependencyTracker::clearSessionTraces();
        DependencyTracker parent(pools);

        // Parent's own deps (before child)
        for (auto & d : parentDeps)
            DependencyTracker::record(d);

        // Child evaluates fresh — records into sessionTraces
        uint32_t childStart = DependencyTracker::sessionTraces.size();
        for (auto & d : childDeps)
            DependencyTracker::record(d);
        uint32_t childEnd = DependencyTracker::sessionTraces.size();

        // Exclude child range (what ChildRangeExcluder does)
        parent.excludeChildRange(childStart, childEnd);

        return keys(pools, parent.collectTraces());
    }

    /// Same as runWithFreshChild but the child's deps are replayed via
    /// replayedRanges (simulating a cached child replaying its trace).
    std::vector<std::string> runWithCachedChild(
        const std::vector<Dep> & parentDeps,
        const std::vector<Dep> & childReplayedDeps)
    {
        DependencyTracker::clearSessionTraces();
        DependencyTracker parent(pools);

        // Parent's own deps
        for (auto & d : parentDeps)
            DependencyTracker::record(d);

        // Child replays cached deps — these go into sessionTraces AND
        // are added as a replayed range on the parent's tracker
        uint32_t childStart = DependencyTracker::sessionTraces.size();
        for (auto & d : childReplayedDeps)
            DependencyTracker::record(d);
        uint32_t childEnd = DependencyTracker::sessionTraces.size();

        parent.replayedRanges.push_back(
            DepRange{&DependencyTracker::sessionTraces, childStart, childEnd});

        // Exclude child range
        parent.excludeChildRange(childStart, childEnd);

        return keys(pools, parent.collectTraces());
    }
};

// ═════════════════════════════════════════════════════════════════════
// Test 1: Parent deps stable when child evaluates fresh
// ═════════════════════════════════════════════════════════════════════

TEST_F(DepStabilityTest, ParentDepsStable_ChildFreshEval)
{
    std::vector<Dep> parentDeps = {
        makeContentDep(pools, "/parent/a.nix", "pa"),
        makeContentDep(pools, "/parent/b.nix", "pb"),
    };

    // Run 1: child has 4 deps
    auto run1 = runWithFreshChild(parentDeps, {
        makeContentDep(pools, "/child/f1.nix", "c1"),
        makeContentDep(pools, "/child/f2.nix", "c2"),
        makeContentDep(pools, "/child/f3.nix", "c3"),
        makeContentDep(pools, "/child/f4.nix", "c4"),
    });

    // Run 2: child has different deps (different caching state)
    auto run2 = runWithFreshChild(parentDeps, {
        makeContentDep(pools, "/child/f1.nix", "c1"),
        makeContentDep(pools, "/child/f5.nix", "c5"),
    });

    // Parent deps must be identical regardless of child's dep set
    EXPECT_EQ(run1, run2);
    EXPECT_EQ(run1, (std::vector<std::string>{"/parent/a.nix", "/parent/b.nix"}));
}

// ═════════════════════════════════════════════════════════════════════
// Test 2: Parent deps stable when child replays cached deps
// ═════════════════════════════════════════════════════════════════════

TEST_F(DepStabilityTest, ParentDepsStable_ChildCachedReplay)
{
    std::vector<Dep> parentDeps = {
        makeContentDep(pools, "/parent/x.nix", "px"),
    };

    // Run 1: child evaluates fresh with N deps
    auto run1 = runWithFreshChild(parentDeps, {
        makeContentDep(pools, "/child/a.nix", "ca"),
        makeContentDep(pools, "/child/b.nix", "cb"),
        makeContentDep(pools, "/child/c.nix", "cc"),
    });

    // Run 2: child is cached, replays M != N deps
    auto run2 = runWithCachedChild(parentDeps, {
        makeContentDep(pools, "/child/a.nix", "ca"),
        makeContentDep(pools, "/child/d.nix", "cd"),
    });

    // Parent sees only its own deps in both cases
    EXPECT_EQ(run1, run2);
    EXPECT_EQ(run1, (std::vector<std::string>{"/parent/x.nix"}));
}

// ═════════════════════════════════════════════════════════════════════
// Test 3: Oscillation pattern — three consecutive runs
// ═════════════════════════════════════════════════════════════════════

TEST_F(DepStabilityTest, OscillationPattern_ThreeConsecutiveRuns)
{
    std::vector<Dep> parentDeps = {
        makeContentDep(pools, "/parent/root.nix", "pr"),
    };

    // Run 1 (cold): child evaluates fresh, records {f1, f2, f3, f4}
    auto run1 = runWithFreshChild(parentDeps, {
        makeContentDep(pools, "/child/f1.nix", "c1"),
        makeContentDep(pools, "/child/f2.nix", "c2"),
        makeContentDep(pools, "/child/f3.nix", "c3"),
        makeContentDep(pools, "/child/f4.nix", "c4"),
    });

    // Run 2 (warm): child verify-replays subset {f1, f2}
    auto run2 = runWithCachedChild(parentDeps, {
        makeContentDep(pools, "/child/f1.nix", "c1"),
        makeContentDep(pools, "/child/f2.nix", "c2"),
    });

    // Run 3 (re-eval): child evaluates fresh again with {f1, f2, f5}
    auto run3 = runWithFreshChild(parentDeps, {
        makeContentDep(pools, "/child/f1.nix", "c1"),
        makeContentDep(pools, "/child/f2.nix", "c2"),
        makeContentDep(pools, "/child/f5.nix", "c5"),
    });

    // All three runs produce identical parent deps
    EXPECT_EQ(run1, run2);
    EXPECT_EQ(run2, run3);
    EXPECT_EQ(run1, (std::vector<std::string>{"/parent/root.nix"}));
}

// ═════════════════════════════════════════════════════════════════════
// Test 4: Without exclusion, parent deps vary (negative test)
// ═════════════════════════════════════════════════════════════════════

TEST_F(DepStabilityTest, NoExclusion_ParentDepsVary)
{
    // Run 1: child has 3 deps, NO exclusion applied
    {
        DependencyTracker::clearSessionTraces();
        DependencyTracker parent(pools);
        DependencyTracker::record(makeContentDep(pools,"/parent/p.nix", "pp"));
        DependencyTracker::record(makeContentDep(pools,"/child/a.nix", "ca"));
        DependencyTracker::record(makeContentDep(pools,"/child/b.nix", "cb"));
        DependencyTracker::record(makeContentDep(pools,"/child/c.nix", "cc"));
        auto run1 = keys(pools, parent.collectTraces());

        // Run 2: child has different deps, NO exclusion
        DependencyTracker::clearSessionTraces();
        DependencyTracker parent2(pools);
        DependencyTracker::record(makeContentDep(pools,"/parent/p.nix", "pp"));
        DependencyTracker::record(makeContentDep(pools,"/child/x.nix", "cx"));
        auto run2 = keys(pools, parent2.collectTraces());

        // Without exclusion, parent deps DIFFER (this is the bug)
        EXPECT_NE(run1, run2);
        EXPECT_EQ(run1.size(), 4u);  // parent + 3 child deps
        EXPECT_EQ(run2.size(), 2u);  // parent + 1 child dep
    }
}

// ═════════════════════════════════════════════════════════════════════
// Test 5: Multiple children with varying cache states
// ═════════════════════════════════════════════════════════════════════

TEST_F(DepStabilityTest, MultipleChildren_VaryingCacheState)
{
    std::vector<Dep> parentDeps = {
        makeContentDep(pools, "/parent/main.nix", "pm"),
    };

    // Run 1: C1 fresh (100 deps), C2 cached (50 deps)
    auto run1 = [&]() {
        DependencyTracker::clearSessionTraces();
        DependencyTracker parent(pools);

        for (auto & d : parentDeps)
            DependencyTracker::record(d);

        // C1 fresh: 100 deps
        uint32_t c1Start = DependencyTracker::sessionTraces.size();
        for (int i = 0; i < 100; i++)
            DependencyTracker::record(
                makeContentDep(pools,"/c1/f" + std::to_string(i) + ".nix", "c1-" + std::to_string(i)));
        uint32_t c1End = DependencyTracker::sessionTraces.size();
        parent.excludeChildRange(c1Start, c1End);

        // C2 cached: 50 replayed deps
        uint32_t c2Start = DependencyTracker::sessionTraces.size();
        for (int i = 0; i < 50; i++)
            DependencyTracker::record(
                makeContentDep(pools,"/c2/g" + std::to_string(i) + ".nix", "c2-" + std::to_string(i)));
        uint32_t c2End = DependencyTracker::sessionTraces.size();
        parent.replayedRanges.push_back(
            DepRange{&DependencyTracker::sessionTraces, c2Start, c2End});
        parent.excludeChildRange(c2Start, c2End);

        return keys(pools, parent.collectTraces());
    }();

    // Run 2: C1 cached (30 deps), C2 fresh (80 deps)
    auto run2 = [&]() {
        DependencyTracker::clearSessionTraces();
        DependencyTracker parent(pools);

        for (auto & d : parentDeps)
            DependencyTracker::record(d);

        // C1 cached: 30 replayed deps
        uint32_t c1Start = DependencyTracker::sessionTraces.size();
        for (int i = 0; i < 30; i++)
            DependencyTracker::record(
                makeContentDep(pools,"/c1/f" + std::to_string(i) + ".nix", "c1-" + std::to_string(i)));
        uint32_t c1End = DependencyTracker::sessionTraces.size();
        parent.replayedRanges.push_back(
            DepRange{&DependencyTracker::sessionTraces, c1Start, c1End});
        parent.excludeChildRange(c1Start, c1End);

        // C2 fresh: 80 deps
        uint32_t c2Start = DependencyTracker::sessionTraces.size();
        for (int i = 0; i < 80; i++)
            DependencyTracker::record(
                makeContentDep(pools,"/c2/g" + std::to_string(i) + ".nix", "c2-" + std::to_string(i)));
        uint32_t c2End = DependencyTracker::sessionTraces.size();
        parent.excludeChildRange(c2Start, c2End);

        return keys(pools, parent.collectTraces());
    }();

    EXPECT_EQ(run1, run2);
    EXPECT_EQ(run1, (std::vector<std::string>{"/parent/main.nix"}));
}

// ═════════════════════════════════════════════════════════════════════
// Test 6: Exclusion composes with replayed ranges within child scope
// ═════════════════════════════════════════════════════════════════════

TEST_F(DepStabilityTest, ExclusionPlusReplayedRanges_Composable)
{
    DependencyTracker::clearSessionTraces();
    DependencyTracker parent(pools);

    DependencyTracker::record(makeContentDep(pools,"/parent/p.nix", "pp"));

    // Child evaluates and also triggers epoch replay within its scope.
    // Both the child's fresh deps AND replayed deps fall within the
    // excluded range and must be filtered.
    uint32_t childStart = DependencyTracker::sessionTraces.size();
    DependencyTracker::record(makeContentDep(pools,"/child/fresh1.nix", "cf1"));
    DependencyTracker::record(makeContentDep(pools,"/child/fresh2.nix", "cf2"));

    // Simulate epoch replay within child's scope: adds a replayed range
    // to parent that falls within the child's excluded range.
    uint32_t replayStart = DependencyTracker::sessionTraces.size();
    DependencyTracker::record(makeContentDep(pools,"/child/replayed.nix", "cr"));
    uint32_t replayEnd = DependencyTracker::sessionTraces.size();
    parent.replayedRanges.push_back(
        DepRange{&DependencyTracker::sessionTraces, replayStart, replayEnd});

    uint32_t childEnd = DependencyTracker::sessionTraces.size();

    // More parent deps after child
    DependencyTracker::record(makeContentDep(pools,"/parent/q.nix", "pq"));

    parent.excludeChildRange(childStart, childEnd);

    auto deps = keys(pools, parent.collectTraces());
    // Only parent deps survive — child's fresh and replayed deps excluded
    EXPECT_EQ(deps, (std::vector<std::string>{"/parent/p.nix", "/parent/q.nix"}));
}

// ═════════════════════════════════════════════════════════════════════
// Test 7: Per-sibling dep chain detects file change (TraceStore level)
//
// Reproduces the relative-paths-lockfile regression mechanism:
// Child y has per-sibling ParentContext dep on sibling x.
// Sibling x has a Content dep on a file.
// When the file changes, the chain y→x→file must detect it.
// ═════════════════════════════════════════════════════════════════════

class DepStabilityStoreTest : public TraceStoreFixture
{
protected:
    static constexpr int64_t testCtxHash = 0xDEADBEEF42424242;
    TraceStore makeDb() { return TraceStore(state.symbols, *state.traceCtx->pools,
        state.traceCtx->getVocabStore(state.symbols), testCtxHash); }
};

TEST_F(DepStabilityStoreTest, PerSiblingChain_FileChange_Detected)
{
    // Simulate: rec { x = readFile f; y = x - 1; }
    // x reads a file → Content dep. y accesses x → per-sibling ParentContext dep.
    // When the file changes, y must detect it via the chain:
    //   y → ParentContext(x) → verify(x) → Content(file) fails → y re-evaluates.

    ScopedCacheDir cacheDir;
    TempExtFile dataFile("json", "11");
    auto db = makeDb();

    // Record ROOT trace with zero deps (typical for ROOT)
    // rootPath() provided by fixture
    db.record(rootPath(), CachedResult(attrs_t{}), {}, true);
    auto rootHash = db.getCurrentTraceHash(rootPath());
    ASSERT_TRUE(rootHash.has_value());

    // Record "x" trace with Content dep on the data file + ParentContext on ROOT
    auto xPathId = vpath({"x"});
    auto fileHash = StatHashStore::instance().depHashFile(
        SourcePath(getFSSourceAccessor(), CanonPath(dataFile.path.string())));
    std::vector<Dep> xDeps = {
        {{DepType::Content, pools().intern<DepSourceId>(""), pools().intern<DepKeyId>(dataFile.path.string())}, DepHashValue(fileHash)},
        makeParentContextDep(rootPath(), *rootHash),
    };
    db.record(xPathId, CachedResult(int_t{NixInt(11)}), xDeps, false);
    auto xHash = db.getCurrentTraceHash(xPathId);
    ASSERT_TRUE(xHash.has_value());

    // Record "y" trace with per-sibling ParentContext dep on x
    auto yPathId = vpath({"y"});
    std::vector<Dep> yDeps = {
        makeParentContextDep(vpath({"x"}), *xHash),
    };
    db.record(yPathId, CachedResult(int_t{NixInt(10)}), yDeps, false);

    // Before file change: y should verify (chain is valid)
    db.clearSessionCaches();
    auto yVerify1 = db.verify(yPathId, {}, state);
    ASSERT_TRUE(yVerify1.has_value()) << "y should verify before file change";

    // Change the data file
    dataFile.modify("13");
    getFSSourceAccessor()->invalidateCache(CanonPath(dataFile.path.string()));
    StatHashStore::instance().clear();

    // After file change: y should FAIL verification
    // Chain: y → ParentContext(x) → verify(x) → Content(file) changed → FAIL
    db.clearSessionCaches();
    auto yVerify2 = db.verify(yPathId, {}, state);
    EXPECT_FALSE(yVerify2.has_value())
        << "y must detect file change via per-sibling dep chain";
}

TEST_F(DepStabilityStoreTest, PerSiblingChain_NoChange_StillValid)
{
    // Same setup as above, but file doesn't change. y should verify.
    ScopedCacheDir cacheDir;
    TempExtFile dataFile("json", "42");
    auto db = makeDb();

    db.record(rootPath(), CachedResult(attrs_t{}), {}, true);
    auto rootHash = db.getCurrentTraceHash(rootPath());
    ASSERT_TRUE(rootHash.has_value());

    auto xPathId = vpath({"x"});
    auto fileHash = StatHashStore::instance().depHashFile(
        SourcePath(getFSSourceAccessor(), CanonPath(dataFile.path.string())));
    db.record(xPathId, CachedResult(int_t{NixInt(42)}),
        {{{DepType::Content, pools().intern<DepSourceId>(""), pools().intern<DepKeyId>(dataFile.path.string())}, DepHashValue(fileHash)},
         makeParentContextDep(rootPath(), *rootHash)}, false);
    auto xHash = db.getCurrentTraceHash(xPathId);
    ASSERT_TRUE(xHash.has_value());

    auto yPathId = vpath({"y"});
    db.record(yPathId, CachedResult(int_t{NixInt(41)}),
        {makeParentContextDep(vpath({"x"}), *xHash)}, false);

    db.clearSessionCaches();
    auto yVerify = db.verify(yPathId, {}, state);
    EXPECT_TRUE(yVerify.has_value()) << "y should verify when file unchanged";
}

// ═════════════════════════════════════════════════════════════════════
// Test 8: Recovery must not bypass recursive ParentContext verification
//
// Regression test for the relative-paths-lockfile failure:
// verifyTrace correctly fails (sibling's deps changed), but recovery()
// finds a historical trace where the ParentContext dep hash matches
// WITHOUT recursively verifying the sibling's trace is valid.
// ═════════════════════════════════════════════════════════════════════

TEST_F(DepStabilityStoreTest, Recovery_MustNotBypassRecursiveParentContextVerification)
{
    ScopedCacheDir cacheDir;
    TempExtFile dataFile("nix", "content_v1");
    auto db = makeDb();

    // ROOT: stable (no deps that change)
    db.record(rootPath(), CachedResult(attrs_t{}), {}, true);
    auto rootHash = db.getCurrentTraceHash(rootPath());
    ASSERT_TRUE(rootHash.has_value());

    // x: Content dep on the data file + ParentContext on ROOT
    auto xPathId = vpath({"x"});
    auto fileHash = StatHashStore::instance().depHashFile(
        SourcePath(getFSSourceAccessor(), CanonPath(dataFile.path.string())));
    db.record(xPathId, CachedResult(string_t{"x_v1", {}}),
        {{{DepType::Content, pools().intern<DepSourceId>(""), pools().intern<DepKeyId>(dataFile.path.string())}, DepHashValue(fileHash)},
         makeParentContextDep(rootPath(), *rootHash)},
        false);
    auto xHash = db.getCurrentTraceHash(xPathId);
    ASSERT_TRUE(xHash.has_value());

    // y: per-sibling ParentContext dep on x only
    auto yPathId = vpath({"y"});
    db.record(yPathId, CachedResult(string_t{"y_v1", {}}),
        {makeParentContextDep(vpath({"x"}), *xHash)},
        false);

    // Verify y before change — should pass
    db.clearSessionCaches();
    ASSERT_TRUE(db.verify(yPathId, {}, state).has_value());

    // Change the file that x depends on
    dataFile.modify("content_v2");
    getFSSourceAccessor()->invalidateCache(CanonPath(dataFile.path.string()));
    StatHashStore::instance().clear();
    db.clearSessionCaches();

    // Verify y after change — must FAIL
    // verifyTrace correctly detects x's Content dep failure via recursive
    // ParentContext verification. But recovery() must NOT find a stale
    // historical trace where the ParentContext hash matches without
    // recursive verification.
    auto result = db.verify(yPathId, {}, state);
    EXPECT_FALSE(result.has_value())
        << "recovery must not bypass recursive ParentContext verification";
}

// ═════════════════════════════════════════════════════════════════════
// Test 9: Unrelated input change must NOT invalidate sibling's trace
//
// Models the scenario: a flake has inputs depA and depB. Attribute x
// only uses depA. Changing the lockfile to update depB should NOT
// invalidate x's trace, because x has no dep on depB.
// ═════════════════════════════════════════════════════════════════════

TEST_F(DepStabilityStoreTest, UnrelatedInputChange_DoesNotInvalidateSibling)
{
    ScopedCacheDir cacheDir;
    TempExtFile depAFile("nix", "{ x = 11; }");
    TempExtFile depBFile("nix", "{ z = 99; }");
    auto db = makeDb();

    // ROOT trace: no deps (stable)
    db.record(rootPath(), CachedResult(attrs_t{}), {}, true);
    auto rootTraceHash = db.getCurrentTraceHash(rootPath());
    ASSERT_TRUE(rootTraceHash.has_value());

    // "x" trace: Content dep on depA file + ParentContext on ROOT
    auto depAHash = StatHashStore::instance().depHashFile(
        SourcePath(getFSSourceAccessor(), CanonPath(depAFile.path.string())));
    auto xPathId = vpath({"x"});
    db.record(xPathId, CachedResult(int_t{NixInt(11)}),
        {{{DepType::Content, pools().intern<DepSourceId>(""), pools().intern<DepKeyId>(depAFile.path.string())}, DepHashValue(depAHash)},
         makeParentContextDep(rootPath(), *rootTraceHash)},
        false);
    auto xTraceHash = db.getCurrentTraceHash(xPathId);
    ASSERT_TRUE(xTraceHash.has_value());

    // "z" trace: Content dep on depB file + ParentContext on ROOT
    auto depBHash = StatHashStore::instance().depHashFile(
        SourcePath(getFSSourceAccessor(), CanonPath(depBFile.path.string())));
    auto zPathId = vpath({"z"});
    db.record(zPathId, CachedResult(int_t{NixInt(99)}),
        {{{DepType::Content, pools().intern<DepSourceId>(""), pools().intern<DepKeyId>(depBFile.path.string())}, DepHashValue(depBHash)},
         makeParentContextDep(rootPath(), *rootTraceHash)},
        false);

    // "y" trace: per-sibling dep on x only (y = x - 1, doesn't touch z)
    auto yPathId = vpath({"y"});
    db.record(yPathId, CachedResult(int_t{NixInt(10)}),
        {makeParentContextDep(vpath({"x"}), *xTraceHash)},
        false);

    // Simulate lockfile change: depB changes, depA stays the same.
    depBFile.modify("{ z = 100; }");
    getFSSourceAccessor()->invalidateCache(CanonPath(depBFile.path.string()));
    StatHashStore::instance().clear();
    db.clearSessionCaches();

    // y should still verify — y depends on x, x depends on depA, depA is unchanged
    auto yVerify = db.verify(yPathId, {}, state);
    EXPECT_TRUE(yVerify.has_value())
        << "y must NOT be invalidated by unrelated depB change";

    // z should be invalidated — z depends on depB which changed
    auto zVerify = db.verify(zPathId, {}, state);
    EXPECT_FALSE(zVerify.has_value())
        << "z must be invalidated by depB change";

    // x should still verify — x depends on depA which is unchanged
    auto xVerify = db.verify(xPathId, {}, state);
    EXPECT_TRUE(xVerify.has_value())
        << "x must NOT be invalidated by unrelated depB change";
}

// ═════════════════════════════════════════════════════════════════════
// Test 9: Full integration — rec attrset with file-reading sibling
//
// Uses TraceCacheFixture to exercise the complete flow:
// ChildRangeExcluder + SiblingAccessTracker + ParentContext deps.
// ═════════════════════════════════════════════════════════════════════

class DepStabilityIntegrationTest : public TraceCacheFixture
{
public:
    DepStabilityIntegrationTest()
    {
        testFingerprint = hashString(HashAlgorithm::SHA256, "dep-stability-integration");
    }
};

TEST_F(DepStabilityIntegrationTest, SiblingFileChange_PropagatedThroughDepChain)
{
    // rec { x = fromJSON(readFile f); y = x - 1; }
    // When f changes from 11 to 13, y must change from 10 to 12.
    // The file dep is on x (origExpr sibling). y detects it via
    // per-sibling ParentContext dep on x's trace hash.

    TempExtFile dataFile("json", "11");

    std::string expr = fmt(R"(
        rec {
            x = builtins.fromJSON (builtins.readFile "%s");
            y = x - 1;
        }
    )", dataFile.path.string());

    // Eval 1: cold cache — ROOT + attributes evaluate fresh
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * y = root.attrs()->get(state.symbols.create("y"));
        state.forceValue(*y->value, noPos);
        EXPECT_EQ(y->value->integer().value, 10);
    }

    // Eval 2: warm cache — ROOT materializes TracedExpr children,
    // x and y evaluate as TracedExprs (their traces are recorded)
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * y = root.attrs()->get(state.symbols.create("y"));
        state.forceValue(*y->value, noPos);
        EXPECT_EQ(y->value->integer().value, 10);
    }

    // Change the file (simulate lockfile update)
    dataFile.modify("13");
    invalidateFileCache(dataFile.path);

    // Eval 3: warm cache — must detect file change and produce new result
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * y = root.attrs()->get(state.symbols.create("y"));
        state.forceValue(*y->value, noPos);
        EXPECT_EQ(y->value->integer().value, (int64_t)12)
            << "y must detect file change via per-sibling dep chain on x";
    }
}

// ═════════════════════════════════════════════════════════════════════
// Epoch map stability tests
//
// These test the interaction between SuspendDepTracking and the epoch
// map (recordThunkDeps / replayMemoizedDeps). The bug: when a thunk
// is forced during SuspendDepTracking, forceValue() does NOT call
// recordThunkDeps (because isActive() is false). The epochMap has no
// entry. When a later tracker accesses the same Value, replay finds
// nothing. If the deps from SuspendDepTracking happen to be in the
// later tracker's session range (they ARE, since record() always
// appends to sessionTraces), the immediate deps are preserved. But
// for NESTED thunks (thunk W depends on thunk V forced during
// suspension), W's epoch range doesn't include V's deps, and any
// future replay of W also misses V's deps. This creates cascading
// dep loss that varies by evaluation order.
// ═════════════════════════════════════════════════════════════════════

class EpochStabilityTest : public ::testing::Test
{
protected:
    InterningPools pools;
    EvalTraceContext ctx;
    void SetUp() override { DependencyTracker::clearSessionTraces(); }
    void TearDown() override { DependencyTracker::clearSessionTraces(); }
};

// ── Negative test: shows the bug ─────────────────────────────────────

TEST_F(EpochStabilityTest, SuspendedThunk_EpochAlwaysRecorded)
{
    // After the fix: forceValue() always calls recordThunkDeps
    // regardless of isActive(). Simulate this by always recording.
    // The epochMap entry enables replay even for thunks forced
    // during SuspendDepTracking.

    Value v;
    v.mkInt(42);

    // Phase 1: outer tracker, SuspendDepTracking, force V
    {
        DependencyTracker outer(pools);
        uint32_t epochStart = DependencyTracker::sessionTraces.size();
        {
            SuspendDepTracking suspend;
            DependencyTracker::record(makeContentDep(pools,"/lib/default.nix", "lib"));
            DependencyTracker::record(makeContentDep(pools,"/lib/attrsets.nix", "attrs"));
        }
        // Fix: recordThunkDeps called regardless of isActive()
        ctx.recordThunkDeps(v, epochStart);
    }

    // Phase 2: new tracker replays V successfully
    {
        DependencyTracker tracker(pools);
        DependencyTracker::record(makeContentDep(pools,"/closure.nix", "closure"));
        ctx.replayMemoizedDeps(v);

        auto deps = tracker.collectTraces();
        // V's deps are replayed → 3 total deps
        EXPECT_EQ(deps.size(), 3u)
            << "epochMap entry from suspended forcing enables replay";
    }
}

// ── Positive test: shows correct behavior with epochMap entry ────────

TEST_F(EpochStabilityTest, SuspendedThunk_WithEpochEntry_ReplaySucceeds)
{
    // Same scenario, but recordThunkDeps IS called (simulating the fix).
    // The new tracker successfully replays V's deps.

    Value v;
    v.mkInt(42);

    // Phase 1: outer tracker, SuspendDepTracking, force V, record epoch
    {
        DependencyTracker outer(pools);
        uint32_t epochStart = DependencyTracker::sessionTraces.size();
        {
            SuspendDepTracking suspend;
            DependencyTracker::record(makeContentDep(pools,"/lib/default.nix", "lib"));
            DependencyTracker::record(makeContentDep(pools,"/lib/attrsets.nix", "attrs"));
        }
        // FIX: recordThunkDeps called regardless of isActive()
        ctx.recordThunkDeps(v, epochStart);
    }

    // Phase 2: new tracker replays V successfully
    {
        DependencyTracker tracker(pools);
        DependencyTracker::record(makeContentDep(pools,"/closure.nix", "closure"));
        ctx.replayMemoizedDeps(v);

        auto deps = tracker.collectTraces();
        // V's deps are replayed via epochMap → 3 total deps
        EXPECT_EQ(deps.size(), 3u)
            << "with epochMap entry, replay adds V's deps";
    }
}

// ── Stability test: dep set is same regardless of when V was forced ──

TEST_F(EpochStabilityTest, DepSetStable_RegardlessOfSuspensionState)
{
    // The dep set collected by a child tracker should be IDENTICAL
    // whether a shared thunk V was forced:
    //   (A) in a prior tracker scope (with epochMap entry → replay), or
    //   (B) during SuspendDepTracking (with epochMap entry → replay)
    //
    // Without the fix, case B produces a different dep set because
    // replay doesn't work (no epochMap entry).

    Value v;
    v.mkInt(42);

    // Case A: V forced in prior tracker scope (normal case)
    auto depsA = [&]() {
        DependencyTracker::clearSessionTraces();
        ctx.epochMap.clear();

        // Prior scope forces V
        {
            DependencyTracker prior(pools);
            uint32_t epochStart = DependencyTracker::sessionTraces.size();
            DependencyTracker::record(makeContentDep(pools,"/lib/default.nix", "lib"));
            DependencyTracker::record(makeContentDep(pools,"/lib/attrsets.nix", "attrs"));
            ctx.recordThunkDeps(v, epochStart);
        }

        // Child scope records own deps + replays V
        DependencyTracker child(pools);
        DependencyTracker::record(makeContentDep(pools,"/closure.nix", "closure"));
        ctx.replayMemoizedDeps(v);
        return keys(pools, child.collectTraces());
    }();

    // Case B: V forced during SuspendDepTracking (the callFlake scenario)
    auto depsB = [&]() {
        DependencyTracker::clearSessionTraces();
        ctx.epochMap.clear();

        // Outer scope with suspension forces V
        {
            DependencyTracker outer(pools);
            uint32_t epochStart = DependencyTracker::sessionTraces.size();
            {
                SuspendDepTracking suspend;
                DependencyTracker::record(makeContentDep(pools,"/lib/default.nix", "lib"));
                DependencyTracker::record(makeContentDep(pools,"/lib/attrsets.nix", "attrs"));
            }
            // FIX: recordThunkDeps called regardless of isActive()
            ctx.recordThunkDeps(v, epochStart);
        }

        // Child scope records own deps + replays V
        DependencyTracker child(pools);
        DependencyTracker::record(makeContentDep(pools,"/closure.nix", "closure"));
        ctx.replayMemoizedDeps(v);
        return keys(pools, child.collectTraces());
    }();

    // Both should produce the same dep set
    EXPECT_EQ(depsA, depsB)
        << "dep set must be identical regardless of when shared thunk was forced";
}

// ── Nested thunk test: W depends on V, both under suspension ─────────

TEST_F(EpochStabilityTest, NestedThunks_SuspendedForcing_DepsPreserved)
{
    // Simulates: callFlake forces V, then W (which depends on V).
    // Both happen during SuspendDepTracking.
    // Later, child tracker accesses W → should get W's AND V's deps.

    Value v, w;
    v.mkInt(1);
    w.mkInt(2);

    DependencyTracker::clearSessionTraces();
    ctx.epochMap.clear();

    {
        DependencyTracker outer(pools);
        // V is forced during suspension
        uint32_t vStart = DependencyTracker::sessionTraces.size();
        {
            SuspendDepTracking suspend;
            DependencyTracker::record(makeContentDep(pools,"/v-dep.nix", "v"));
        }
        ctx.recordThunkDeps(v, vStart);

        // W is forced during suspension, W's eval accesses V
        uint32_t wStart = DependencyTracker::sessionTraces.size();
        {
            SuspendDepTracking suspend;
            DependencyTracker::record(makeContentDep(pools,"/w-dep.nix", "w"));
            // W accesses V (already forced) → replay would fire if tracker active
            // During suspension, replay is no-op (activeTracker nullptr)
        }
        ctx.recordThunkDeps(w, wStart);
    }

    // Child tracker accesses W
    DependencyTracker child(pools);
    DependencyTracker::record(makeContentDep(pools,"/child.nix", "c"));
    ctx.replayMemoizedDeps(w);
    // Also replay V (child might access V independently)
    ctx.replayMemoizedDeps(v);

    auto deps = keys(pools, child.collectTraces());
    EXPECT_GE(deps.size(), 3u)
        << "child should get own dep + V's dep + W's dep via replay";
}

} // namespace nix::eval_trace
