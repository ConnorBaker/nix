#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/deps/types.hh"

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

class ChildRangeExclusionTest : public ::testing::Test
{
protected:
    InterningPools pools;
    void SetUp() override { DependencyTracker::clearEpochLog(); }
    void TearDown() override { DependencyTracker::clearEpochLog(); }
};

// ═════════════════════════════════════════════════════════════════════
// Component 1: Structural isolation tests (per-tracker ownDeps)
// ═════════════════════════════════════════════════════════════════════

// ── Positive: inner tracker collects only inner deps ─────────────────

TEST_F(ChildRangeExclusionTest, NestedTrackerIsolation_InnerOnlyHasInnerDeps)
{
    DependencyTracker outer(pools);
    DependencyTracker::record(makeContentDep(pools, "/outer1.nix", "o1"));

    std::vector<Dep> innerDeps;
    {
        DependencyTracker inner(pools);
        DependencyTracker::record(makeContentDep(pools, "/inner1.nix", "i1"));
        DependencyTracker::record(makeContentDep(pools, "/inner2.nix", "i2"));
        innerDeps = inner.collectTraces();
    }

    DependencyTracker::record(makeContentDep(pools, "/outer2.nix", "o2"));

    EXPECT_EQ(keys(pools, innerDeps), (std::vector<std::string>{"/inner1.nix", "/inner2.nix"}));
}

TEST_F(ChildRangeExclusionTest, NestedTrackerIsolation_OuterOnlyHasOuterDeps)
{
    DependencyTracker outer(pools);
    DependencyTracker::record(makeContentDep(pools, "/outer1.nix", "o1"));

    {
        DependencyTracker inner(pools);
        DependencyTracker::record(makeContentDep(pools, "/inner1.nix", "i1"));
        DependencyTracker::record(makeContentDep(pools, "/inner2.nix", "i2"));
        // inner destroyed here; inner deps stay in inner's ownDeps
    }

    DependencyTracker::record(makeContentDep(pools, "/outer2.nix", "o2"));

    auto outerDeps = outer.collectTraces();
    EXPECT_EQ(keys(pools, outerDeps), (std::vector<std::string>{"/outer1.nix", "/outer2.nix"}));
}

TEST_F(ChildRangeExclusionTest, MultipleChildren_EachCollectsOwnDeps)
{
    DependencyTracker parent(pools);
    DependencyTracker::record(makeContentDep(pools, "/p1.nix", "p1"));

    std::vector<Dep> child1Deps, child2Deps;
    {
        DependencyTracker child1(pools);
        DependencyTracker::record(makeContentDep(pools, "/c1a.nix", "c1a"));
        DependencyTracker::record(makeContentDep(pools, "/c1b.nix", "c1b"));
        child1Deps = child1.collectTraces();
    }

    DependencyTracker::record(makeContentDep(pools, "/p2.nix", "p2"));

    {
        DependencyTracker child2(pools);
        DependencyTracker::record(makeContentDep(pools, "/c2a.nix", "c2a"));
        child2Deps = child2.collectTraces();
    }

    DependencyTracker::record(makeContentDep(pools, "/p3.nix", "p3"));

    EXPECT_EQ(keys(pools, child1Deps), (std::vector<std::string>{"/c1a.nix", "/c1b.nix"}));
    EXPECT_EQ(keys(pools, child2Deps), (std::vector<std::string>{"/c2a.nix"}));

    auto parentDeps = parent.collectTraces();
    EXPECT_EQ(keys(pools, parentDeps), (std::vector<std::string>{"/p1.nix", "/p2.nix", "/p3.nix"}));
}

TEST_F(ChildRangeExclusionTest, EmptyTracker_YieldsEmptyCollectTraces)
{
    DependencyTracker tracker(pools);
    // No deps recorded
    EXPECT_TRUE(tracker.collectTraces().empty());
}

TEST_F(ChildRangeExclusionTest, NoExclusionNeeded_ParentDepsAutoClean)
{
    DependencyTracker parent(pools);
    DependencyTracker::record(makeContentDep(pools, "/a.nix", "a"));

    {
        DependencyTracker child(pools);
        DependencyTracker::record(makeContentDep(pools, "/child.nix", "c"));
    }

    DependencyTracker::record(makeContentDep(pools, "/b.nix", "b"));

    // Parent never called excludeChildRange, yet child deps are absent
    auto parentDeps = parent.collectTraces();
    EXPECT_EQ(keys(pools, parentDeps), (std::vector<std::string>{"/a.nix", "/b.nix"}));
}

TEST_F(ChildRangeExclusionTest, DepsBetweenChildren_GoBackToOuter)
{
    DependencyTracker outer(pools);
    DependencyTracker::record(makeContentDep(pools, "/before.nix", "b"));

    {
        DependencyTracker child1(pools);
        DependencyTracker::record(makeContentDep(pools, "/c1.nix", "c1"));
    }

    // This dep is recorded after child1 is destroyed, so activeTracker
    // is back to outer — it goes into outer's ownDeps.
    DependencyTracker::record(makeContentDep(pools, "/between.nix", "mid"));

    {
        DependencyTracker child2(pools);
        DependencyTracker::record(makeContentDep(pools, "/c2.nix", "c2"));
    }

    DependencyTracker::record(makeContentDep(pools, "/after.nix", "a"));

    auto outerDeps = outer.collectTraces();
    EXPECT_EQ(keys(pools, outerDeps),
        (std::vector<std::string>{"/before.nix", "/between.nix", "/after.nix"}));
}

// ── Negative: tracker with all deps excluded is empty ────────────────

TEST_F(ChildRangeExclusionTest, AllDepsInChild_ParentEmpty)
{
    DependencyTracker parent(pools);

    {
        DependencyTracker child(pools);
        DependencyTracker::record(makeContentDep(pools, "/only.nix", "only"));
    }

    // Parent recorded nothing itself
    EXPECT_TRUE(parent.collectTraces().empty());
}

TEST_F(ChildRangeExclusionTest, SingleTrackerNoDeps_FastPath)
{
    DependencyTracker tracker(pools);
    DependencyTracker::record(makeContentDep(pools, "/a.nix", "a"));
    DependencyTracker::record(makeContentDep(pools, "/b.nix", "b"));
    DependencyTracker::record(makeContentDep(pools, "/c.nix", "c"));

    // No nested trackers, no exclusions — all deps preserved
    EXPECT_EQ(keys(pools, tracker.collectTraces()),
        (std::vector<std::string>{"/a.nix", "/b.nix", "/c.nix"}));
}

// ═════════════════════════════════════════════════════════════════════
// Component 2: recordThunkDeps + replayMemoizedDeps epoch behavior
//
// skipEpochRecordFor was removed: TracedExpr thunks now get epoch
// entries like all other thunks. Sibling detection in replayMemoizedDeps
// (via siblingIdentityMap + siblingCallback on RootTrackerScope) handles
// the case where a sibling's epoch entry would cause dep contamination.
// ═════════════════════════════════════════════════════════════════════

class EpochRecordTest : public ::testing::Test
{
protected:
    InterningPools pools;
    EvalTraceContext ctx;
    void SetUp() override { DependencyTracker::clearEpochLog(); }
    void TearDown() override { DependencyTracker::clearEpochLog(); }
};

// ── recordThunkDeps always records when deps exist ───────────────────

TEST_F(EpochRecordTest, AlwaysRecords_SingleValue)
{
    DependencyTracker tracker(pools);
    Value v;
    v.mkInt(42);
    uint32_t epochStart = DependencyTracker::epochLog.size();
    DependencyTracker::record(makeContentDep(pools, "/a.nix", "a"));

    ctx.recordThunkDeps(v, epochStart);

    auto it = ctx.epochMap.find(&v);
    ASSERT_NE(it, ctx.epochMap.end());
    EXPECT_EQ(it->second.start, epochStart);
    EXPECT_EQ(it->second.end, DependencyTracker::epochLog.size());
}

TEST_F(EpochRecordTest, AlwaysRecords_MultipleValues)
{
    DependencyTracker tracker(pools);
    Value v1, v2;
    v1.mkInt(1);
    v2.mkInt(2);

    uint32_t e1 = DependencyTracker::epochLog.size();
    DependencyTracker::record(makeContentDep(pools, "/a.nix", "a"));
    ctx.recordThunkDeps(v1, e1);

    uint32_t e2 = DependencyTracker::epochLog.size();
    DependencyTracker::record(makeContentDep(pools, "/b.nix", "b"));
    ctx.recordThunkDeps(v2, e2);

    // Both values get epoch entries
    auto it1 = ctx.epochMap.find(&v1);
    auto it2 = ctx.epochMap.find(&v2);
    ASSERT_NE(it1, ctx.epochMap.end());
    ASSERT_NE(it2, ctx.epochMap.end());
    EXPECT_EQ(it1->second.start, e1);
    EXPECT_EQ(it2->second.start, e2);
}

TEST_F(EpochRecordTest, EmptyEpoch_NoEntry)
{
    DependencyTracker tracker(pools);
    Value v;
    v.mkInt(42);
    uint32_t epochStart = DependencyTracker::epochLog.size();
    // No deps recorded between epochStart and now

    ctx.recordThunkDeps(v, epochStart);

    // epochStart == epochEnd → no entry (nothing to record)
    EXPECT_EQ(ctx.epochMap.find(&v), ctx.epochMap.end());
}

TEST_F(EpochRecordTest, ResetClearsEpochState)
{
    DependencyTracker tracker(pools);
    Value v;
    v.mkInt(42);
    uint32_t epochStart = DependencyTracker::epochLog.size();
    DependencyTracker::record(makeContentDep(pools, "/a.nix", "a"));
    ctx.recordThunkDeps(v, epochStart);
    ASSERT_FALSE(ctx.epochMap.empty());

    ctx.reset();

    EXPECT_TRUE(ctx.epochMap.empty());
}

// ── replayMemoizedDeps copies deps from epoch range ──────────────────

TEST_F(EpochRecordTest, ReplayMemoizedDeps_CopiesDeps)
{
    // Record deps for a value in an outer tracker
    DependencyTracker outer(pools);
    Value v;
    v.mkInt(42);
    uint32_t epochStart = DependencyTracker::epochLog.size();
    DependencyTracker::record(makeContentDep(pools, "/a.nix", "a"));
    ctx.recordThunkDeps(v, epochStart);

    auto outerDeps = outer.collectTraces();

    // Create a new tracker and replay — deps should be copied
    DependencyTracker inner(pools);
    ctx.replayMemoizedDeps(v);

    auto innerDeps = inner.collectTraces();
    EXPECT_EQ(innerDeps.size(), 1u);
    EXPECT_EQ(keys(pools, innerDeps), (std::vector<std::string>{"/a.nix"}));
}

TEST_F(EpochRecordTest, ReplayMemoizedDeps_SkipsOwnEpoch)
{
    // If the epoch range was recorded during this tracker's lifetime,
    // replay should skip (deps already in ownDeps).
    DependencyTracker tracker(pools);
    Value v;
    v.mkInt(42);
    uint32_t epochStart = DependencyTracker::epochLog.size();
    DependencyTracker::record(makeContentDep(pools, "/a.nix", "a"));
    ctx.recordThunkDeps(v, epochStart);

    // Replay within same tracker — range.start >= epochLogStartIndex
    ctx.replayMemoizedDeps(v);

    // Deps should only appear once (from the direct record, not replay)
    auto deps = tracker.collectTraces();
    EXPECT_EQ(deps.size(), 1u);
}

// ── replayMemoizedDeps sibling detection via siblingCallback ─────────
//
// siblingIdentityMap + siblingCallback on EvalTraceContext enable detection
// of already-materialized siblings in replayMemoizedDeps. When siblingCallback
// is set (by SiblingAccessTracker) and the replayed Value is in
// siblingIdentityMap, the callback is invoked. If it returns true (sibling
// access recorded), replay is skipped (child gets a ParentContext dep instead).
// If it returns false (parent mismatch, no traceId), normal replay proceeds.

namespace {
    thread_local bool callbackInvoked;
    thread_local bool callbackReturnValue;

    bool testCallback(const EvalTraceContext::SiblingIdentity &)
    {
        callbackInvoked = true;
        return callbackReturnValue;
    }
}

TEST_F(EpochRecordTest, SiblingCallback_ReturnsTrue_SkipsReplay)
{
    // Record deps for a value in an outer tracker, then collect to end it.
    DependencyTracker outer(pools);
    Value v;
    v.mkInt(42);
    uint32_t epochStart = DependencyTracker::epochLog.size();
    DependencyTracker::record(makeContentDep(pools, "/a.nix", "a"));
    ctx.recordThunkDeps(v, epochStart);
    auto outerDeps = outer.collectTraces();

    // Set up sibling detection: register v and set callback returning true.
    ctx.siblingIdentityMap.emplace(&v,
        EvalTraceContext::SiblingIdentity{nullptr, nullptr, nullptr, -1, AttrPathId(), nullptr});
    ctx.siblingCallback = &testCallback;
    callbackInvoked = false;
    callbackReturnValue = true;

    // Replay in a new tracker — callback should fire, deps NOT copied.
    DependencyTracker inner(pools);
    ctx.replayMemoizedDeps(v);

    EXPECT_TRUE(callbackInvoked);
    auto innerDeps = inner.collectTraces();
    EXPECT_EQ(innerDeps.size(), 0u)
        << "siblingCallback returned true → replay skipped, no deps copied";
}

TEST_F(EpochRecordTest, SiblingCallback_ReturnsFalse_FallsThrough)
{
    // Record deps in an outer tracker.
    DependencyTracker outer(pools);
    Value v;
    v.mkInt(42);
    uint32_t epochStart = DependencyTracker::epochLog.size();
    DependencyTracker::record(makeContentDep(pools, "/a.nix", "a"));
    ctx.recordThunkDeps(v, epochStart);
    auto outerDeps = outer.collectTraces();

    // Set up sibling detection: register v, callback returns FALSE.
    ctx.siblingIdentityMap.emplace(&v,
        EvalTraceContext::SiblingIdentity{nullptr, nullptr, nullptr, -1, AttrPathId(), nullptr});
    ctx.siblingCallback = &testCallback;
    callbackInvoked = false;
    callbackReturnValue = false;

    // Replay — callback fires but returns false → normal replay.
    DependencyTracker inner(pools);
    ctx.replayMemoizedDeps(v);

    EXPECT_TRUE(callbackInvoked);
    auto innerDeps = inner.collectTraces();
    EXPECT_EQ(innerDeps.size(), 1u)
        << "siblingCallback returned false → fell through to normal dep replay";
    EXPECT_EQ(keys(pools, innerDeps), (std::vector<std::string>{"/a.nix"}));
}

TEST_F(EpochRecordTest, NoSiblingCallback_NormalReplay)
{
    // Record deps in an outer tracker.
    DependencyTracker outer(pools);
    Value v;
    v.mkInt(42);
    uint32_t epochStart = DependencyTracker::epochLog.size();
    DependencyTracker::record(makeContentDep(pools, "/a.nix", "a"));
    ctx.recordThunkDeps(v, epochStart);
    auto outerDeps = outer.collectTraces();

    // Register in siblingIdentityMap but leave siblingCallback null.
    ctx.siblingIdentityMap.emplace(&v,
        EvalTraceContext::SiblingIdentity{nullptr, nullptr, nullptr, -1, AttrPathId(), nullptr});
    ASSERT_EQ(ctx.siblingCallback, nullptr);

    // Replay — no callback set, normal dep replay despite map entry.
    DependencyTracker inner(pools);
    ctx.replayMemoizedDeps(v);

    auto innerDeps = inner.collectTraces();
    EXPECT_EQ(innerDeps.size(), 1u)
        << "siblingCallback null → normal dep replay even when value is in siblingIdentityMap";
}

TEST_F(EpochRecordTest, SiblingCallback_ValueNotInMap_NormalReplay)
{
    // Record deps in an outer tracker.
    DependencyTracker outer(pools);
    Value v;
    v.mkInt(42);
    uint32_t epochStart = DependencyTracker::epochLog.size();
    DependencyTracker::record(makeContentDep(pools, "/a.nix", "a"));
    ctx.recordThunkDeps(v, epochStart);
    auto outerDeps = outer.collectTraces();

    // Set callback but do NOT register v in siblingIdentityMap.
    ctx.siblingCallback = &testCallback;
    callbackInvoked = false;
    callbackReturnValue = true;
    ASSERT_TRUE(ctx.siblingIdentityMap.empty());

    // Replay — value not in map, callback never fires, normal replay.
    DependencyTracker inner(pools);
    ctx.replayMemoizedDeps(v);

    EXPECT_FALSE(callbackInvoked);
    auto innerDeps = inner.collectTraces();
    EXPECT_EQ(innerDeps.size(), 1u)
        << "value not in siblingIdentityMap → callback not invoked, normal replay";
}

TEST_F(EpochRecordTest, ResetClearsSiblingState)
{
    Value v;
    v.mkInt(42);
    ctx.siblingIdentityMap.emplace(&v,
        EvalTraceContext::SiblingIdentity{nullptr, nullptr, nullptr, -1, AttrPathId(), nullptr});
    ctx.siblingCallback = &testCallback;

    ctx.reset();

    EXPECT_TRUE(ctx.siblingIdentityMap.empty());
    EXPECT_EQ(ctx.siblingCallback, nullptr);
}

// ═════════════════════════════════════════════════════════════════════
// Component 3: RAII structural isolation via nested DependencyTracker
// ═════════════════════════════════════════════════════════════════════

// ── Positive: nested tracker lifetime provides automatic isolation ────

TEST_F(ChildRangeExclusionTest, RAII_InnerDepsOnlyInInnerCollectTraces)
{
    DependencyTracker parent(pools);
    DependencyTracker::record(makeContentDep(pools, "/p1.nix", "p1"));

    {
        DependencyTracker child(pools);
        DependencyTracker::record(makeContentDep(pools, "/child.nix", "c"));
        // child destroyed here; its deps are structurally isolated
    }

    DependencyTracker::record(makeContentDep(pools, "/p2.nix", "p2"));
    EXPECT_EQ(keys(pools, parent.collectTraces()), (std::vector<std::string>{"/p1.nix", "/p2.nix"}));
}

TEST_F(ChildRangeExclusionTest, RAII_ExceptionSafety)
{
    DependencyTracker parent(pools);
    DependencyTracker::record(makeContentDep(pools, "/p.nix", "p"));

    try {
        DependencyTracker child(pools);
        DependencyTracker::record(makeContentDep(pools, "/child.nix", "c"));
        throw std::runtime_error("boom");
        // child destroyed via stack unwinding; its deps are lost (in child's ownDeps)
    } catch (...) {}

    // Parent is unaffected — child's deps were in child's own vector
    EXPECT_EQ(keys(pools, parent.collectTraces()), (std::vector<std::string>{"/p.nix"}));
}

TEST_F(ChildRangeExclusionTest, RAII_NestedChildrenCompose)
{
    DependencyTracker grandparent(pools);
    DependencyTracker::record(makeContentDep(pools, "/gp.nix", "gp"));

    std::vector<Dep> childDeps;
    {
        DependencyTracker child(pools);
        DependencyTracker::record(makeContentDep(pools, "/child.nix", "c"));

        {
            DependencyTracker grandchild(pools);
            DependencyTracker::record(makeContentDep(pools, "/gc.nix", "gc"));
            // grandchild collects only /gc.nix
            EXPECT_EQ(keys(pools, grandchild.collectTraces()), (std::vector<std::string>{"/gc.nix"}));
        }

        // child collects only /child.nix (grandchild's deps are in grandchild's ownDeps)
        childDeps = child.collectTraces();
        EXPECT_EQ(keys(pools, childDeps), (std::vector<std::string>{"/child.nix"}));
    }

    DependencyTracker::record(makeContentDep(pools, "/gp2.nix", "gp2"));

    // Grandparent collects only its own deps
    EXPECT_EQ(keys(pools, grandparent.collectTraces()),
        (std::vector<std::string>{"/gp.nix", "/gp2.nix"}));
}

// ── Negative: no nested tracker = all deps go to current tracker ─────

TEST_F(ChildRangeExclusionTest, RAII_NoNestedTracker_AllDepsInCurrent)
{
    DependencyTracker tracker(pools);
    DependencyTracker::record(makeContentDep(pools, "/a.nix", "a"));
    DependencyTracker::record(makeContentDep(pools, "/b.nix", "b"));
    DependencyTracker::record(makeContentDep(pools, "/c.nix", "c"));

    EXPECT_EQ(keys(pools, tracker.collectTraces()),
        (std::vector<std::string>{"/a.nix", "/b.nix", "/c.nix"}));
}

} // namespace nix::eval_trace
