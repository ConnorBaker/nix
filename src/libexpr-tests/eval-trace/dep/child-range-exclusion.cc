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
    void SetUp() override { DependencyTracker::clearSessionTraces(); }
    void TearDown() override { DependencyTracker::clearSessionTraces(); }
};

// ═════════════════════════════════════════════════════════════════════
// Component 1: excludedChildRanges + collectTraces
// ═════════════════════════════════════════════════════════════════════

// ── Positive: exclusion removes exactly the right deps ───────────────

TEST_F(ChildRangeExclusionTest, ExcludeSingleChild_OnlyChildDepsRemoved)
{
    DependencyTracker parent(pools);
    DependencyTracker::record(makeContentDep(pools,"/p1.nix", "p1"));

    uint32_t childStart = DependencyTracker::sessionTraces.size();
    DependencyTracker::record(makeContentDep(pools,"/c1.nix", "c1"));
    DependencyTracker::record(makeContentDep(pools,"/c2.nix", "c2"));
    uint32_t childEnd = DependencyTracker::sessionTraces.size();

    DependencyTracker::record(makeContentDep(pools,"/p2.nix", "p2"));
    parent.excludeChildRange(childStart, childEnd);

    auto deps = parent.collectTraces();
    EXPECT_EQ(keys(pools, deps), (std::vector<std::string>{"/p1.nix", "/p2.nix"}));
}

TEST_F(ChildRangeExclusionTest, ExcludeMultipleChildren_GapPreserved)
{
    DependencyTracker parent(pools);
    DependencyTracker::record(makeContentDep(pools,"/p1.nix", "p1"));

    uint32_t c1s = DependencyTracker::sessionTraces.size();
    DependencyTracker::record(makeContentDep(pools,"/c1.nix", "c1"));
    uint32_t c1e = DependencyTracker::sessionTraces.size();

    DependencyTracker::record(makeContentDep(pools,"/p2.nix", "p2"));

    uint32_t c2s = DependencyTracker::sessionTraces.size();
    DependencyTracker::record(makeContentDep(pools,"/c2a.nix", "c2a"));
    DependencyTracker::record(makeContentDep(pools,"/c2b.nix", "c2b"));
    uint32_t c2e = DependencyTracker::sessionTraces.size();

    DependencyTracker::record(makeContentDep(pools,"/p3.nix", "p3"));

    parent.excludeChildRange(c1s, c1e);
    parent.excludeChildRange(c2s, c2e);

    auto deps = parent.collectTraces();
    EXPECT_EQ(keys(pools, deps), (std::vector<std::string>{"/p1.nix", "/p2.nix", "/p3.nix"}));
}

TEST_F(ChildRangeExclusionTest, ExcludeAll_EmptyResult)
{
    DependencyTracker parent(pools);
    uint32_t s = DependencyTracker::sessionTraces.size();
    DependencyTracker::record(makeContentDep(pools,"/c.nix", "c"));
    uint32_t e = DependencyTracker::sessionTraces.size();
    parent.excludeChildRange(s, e);

    EXPECT_TRUE(parent.collectTraces().empty());
}

TEST_F(ChildRangeExclusionTest, ExcludeReplayedRange_FullyContained)
{
    DependencyTracker parent(pools);
    DependencyTracker::record(makeContentDep(pools,"/p.nix", "p"));

    uint32_t cs = DependencyTracker::sessionTraces.size();
    DependencyTracker::record(makeContentDep(pools,"/c.nix", "c"));
    uint32_t ce = DependencyTracker::sessionTraces.size();

    parent.replayedRanges.push_back(
        DepRange{&DependencyTracker::sessionTraces, cs, ce});
    parent.excludeChildRange(cs, ce);

    // Replayed range is fully within excluded → filtered out
    EXPECT_EQ(keys(pools, parent.collectTraces()), (std::vector<std::string>{"/p.nix"}));
}

// ── Negative: deps that should NOT be excluded ───────────────────────

TEST_F(ChildRangeExclusionTest, NoExclusions_FastPath_AllDepsPreserved)
{
    DependencyTracker tracker(pools);
    DependencyTracker::record(makeContentDep(pools,"/a.nix", "a"));
    DependencyTracker::record(makeContentDep(pools,"/b.nix", "b"));
    DependencyTracker::record(makeContentDep(pools,"/c.nix", "c"));

    EXPECT_TRUE(tracker.excludedChildRanges.empty());
    EXPECT_EQ(keys(pools, tracker.collectTraces()),
        (std::vector<std::string>{"/a.nix", "/b.nix", "/c.nix"}));
}

TEST_F(ChildRangeExclusionTest, EmptyRange_NothingExcluded)
{
    DependencyTracker parent(pools);
    DependencyTracker::record(makeContentDep(pools,"/a.nix", "a"));
    uint32_t pos = DependencyTracker::sessionTraces.size();
    parent.excludeChildRange(pos, pos); // empty range (start == end)

    EXPECT_TRUE(parent.excludedChildRanges.empty());
    EXPECT_EQ(keys(pools, parent.collectTraces()), (std::vector<std::string>{"/a.nix"}));
}

TEST_F(ChildRangeExclusionTest, ExclusionOnOneTracker_DoesNotAffectAnother)
{
    DependencyTracker outer(pools);
    DependencyTracker::record(makeContentDep(pools,"/outer.nix", "o"));
    {
        DependencyTracker inner(pools);
        DependencyTracker::record(makeContentDep(pools,"/inner.nix", "i"));
        // Exclude within inner's scope
        uint32_t s = inner.startIndex;
        uint32_t e = DependencyTracker::sessionTraces.size();
        inner.excludeChildRange(s, e);

        // Inner sees nothing (excluded its own range)
        EXPECT_TRUE(inner.collectTraces().empty());
    }
    // Outer is unaffected — inner's exclusion is per-tracker
    auto outerDeps = outer.collectTraces();
    EXPECT_EQ(keys(pools, outerDeps),
        (std::vector<std::string>{"/outer.nix", "/inner.nix"}));
}

TEST_F(ChildRangeExclusionTest, ReplayedRange_PartialOverlap_NotFiltered)
{
    DependencyTracker parent(pools);
    DependencyTracker::record(makeContentDep(pools,"/p.nix", "p"));

    uint32_t cs = DependencyTracker::sessionTraces.size();
    DependencyTracker::record(makeContentDep(pools,"/c1.nix", "c1"));
    DependencyTracker::record(makeContentDep(pools,"/c2.nix", "c2"));
    uint32_t ce = DependencyTracker::sessionTraces.size();

    // Replayed range extends BEYOND the excluded range (partial overlap)
    DependencyTracker::record(makeContentDep(pools,"/extra.nix", "e"));
    uint32_t beyondEnd = DependencyTracker::sessionTraces.size();
    parent.replayedRanges.push_back(
        DepRange{&DependencyTracker::sessionTraces, cs, beyondEnd});

    // Exclude only [cs, ce), NOT the extra dep
    parent.excludeChildRange(cs, ce);

    auto deps = parent.collectTraces();
    // Session: /p.nix (kept) + /c1, /c2 (excluded) + /extra (kept)
    // Replayed: [cs, beyondEnd) is NOT fully within [cs, ce) → kept
    EXPECT_EQ(keys(pools, deps),
        (std::vector<std::string>{"/p.nix", "/extra.nix", "/c1.nix", "/c2.nix", "/extra.nix"}));
}

TEST_F(ChildRangeExclusionTest, ReplayedRange_OutsideExclusion_Preserved)
{
    DependencyTracker parent(pools);
    DependencyTracker::record(makeContentDep(pools,"/p.nix", "p"));

    uint32_t replayEnd = DependencyTracker::sessionTraces.size();
    parent.replayedRanges.push_back(
        DepRange{&DependencyTracker::sessionTraces, parent.startIndex, replayEnd});

    uint32_t cs = DependencyTracker::sessionTraces.size();
    DependencyTracker::record(makeContentDep(pools,"/c.nix", "c"));
    uint32_t ce = DependencyTracker::sessionTraces.size();
    parent.excludeChildRange(cs, ce);

    auto deps = parent.collectTraces();
    // Session: /p.nix (kept) + /c.nix (excluded)
    // Replayed: [startIndex, replayEnd) is before exclusion → kept
    EXPECT_EQ(keys(pools, deps), (std::vector<std::string>{"/p.nix", "/p.nix"}));
}

TEST_F(ChildRangeExclusionTest, AdjacentExclusions_GapNotSwallowed)
{
    DependencyTracker parent(pools);

    uint32_t c1s = DependencyTracker::sessionTraces.size();
    DependencyTracker::record(makeContentDep(pools,"/c1.nix", "c1"));
    uint32_t c1e = DependencyTracker::sessionTraces.size();

    // Single dep between two excluded ranges — must survive
    DependencyTracker::record(makeContentDep(pools,"/between.nix", "b"));

    uint32_t c2s = DependencyTracker::sessionTraces.size();
    DependencyTracker::record(makeContentDep(pools,"/c2.nix", "c2"));
    uint32_t c2e = DependencyTracker::sessionTraces.size();

    parent.excludeChildRange(c1s, c1e);
    parent.excludeChildRange(c2s, c2e);

    EXPECT_EQ(keys(pools, parent.collectTraces()), (std::vector<std::string>{"/between.nix"}));
}

// ═════════════════════════════════════════════════════════════════════
// Component 2: skipEpochRecordFor + recordThunkDeps
// ═════════════════════════════════════════════════════════════════════

class SkipEpochRecordTest : public ::testing::Test
{
protected:
    InterningPools pools;
    EvalTraceContext ctx;
    void SetUp() override { DependencyTracker::clearSessionTraces(); }
    void TearDown() override { DependencyTracker::clearSessionTraces(); }
};

// ── Positive: skip prevents epoch map entry ──────────────────────────

TEST_F(SkipEpochRecordTest, SkipMatchingValue_NoEpochEntry)
{
    DependencyTracker tracker(pools);
    Value v;
    v.mkInt(42);
    uint32_t epochStart = DependencyTracker::sessionTraces.size();
    DependencyTracker::record(makeContentDep(pools,"/a.nix", "a"));

    ctx.skipEpochRecordFor = &v;
    ctx.recordThunkDeps(v, epochStart);

    EXPECT_EQ(ctx.epochMap.find(&v), ctx.epochMap.end());
    EXPECT_EQ(ctx.skipEpochRecordFor, nullptr); // consumed
}

TEST_F(SkipEpochRecordTest, SkipConsumedOnce_SecondCallRecords)
{
    DependencyTracker tracker(pools);
    Value v;
    v.mkInt(42);

    ctx.skipEpochRecordFor = &v;

    uint32_t e1 = DependencyTracker::sessionTraces.size();
    DependencyTracker::record(makeContentDep(pools,"/a.nix", "a"));
    ctx.recordThunkDeps(v, e1);
    EXPECT_EQ(ctx.epochMap.find(&v), ctx.epochMap.end()); // skipped

    uint32_t e2 = DependencyTracker::sessionTraces.size();
    DependencyTracker::record(makeContentDep(pools,"/b.nix", "b"));
    ctx.recordThunkDeps(v, e2);

    auto it = ctx.epochMap.find(&v);
    ASSERT_NE(it, ctx.epochMap.end()); // recorded
    EXPECT_EQ(it->second.start, e2);
    EXPECT_EQ(it->second.end, DependencyTracker::sessionTraces.size());
}

TEST_F(SkipEpochRecordTest, ResetClearsFlag)
{
    Value v;
    v.mkInt(42);
    ctx.skipEpochRecordFor = &v;
    ctx.reset();
    EXPECT_EQ(ctx.skipEpochRecordFor, nullptr);
    EXPECT_TRUE(ctx.epochMap.empty());
}

// ── Negative: non-matching values still get recorded ─────────────────

TEST_F(SkipEpochRecordTest, NormalRecording_NoSkip)
{
    DependencyTracker tracker(pools);
    Value v;
    v.mkInt(42);
    uint32_t epochStart = DependencyTracker::sessionTraces.size();
    DependencyTracker::record(makeContentDep(pools,"/a.nix", "a"));

    EXPECT_EQ(ctx.skipEpochRecordFor, nullptr); // no skip set
    ctx.recordThunkDeps(v, epochStart);

    auto it = ctx.epochMap.find(&v);
    ASSERT_NE(it, ctx.epochMap.end());
    EXPECT_EQ(it->second.start, epochStart);
    EXPECT_EQ(it->second.end, DependencyTracker::sessionTraces.size());
}

TEST_F(SkipEpochRecordTest, SkipTargetsOneValue_OthersUnaffected)
{
    DependencyTracker tracker(pools);
    Value target, other;
    target.mkInt(1);
    other.mkInt(2);

    ctx.skipEpochRecordFor = &target;

    uint32_t epochStart = DependencyTracker::sessionTraces.size();
    DependencyTracker::record(makeContentDep(pools,"/a.nix", "a"));
    ctx.recordThunkDeps(other, epochStart);

    // 'other' was recorded despite skip being set (different address)
    auto it = ctx.epochMap.find(&other);
    ASSERT_NE(it, ctx.epochMap.end());
    EXPECT_EQ(it->second.start, epochStart);

    // target was never recorded
    EXPECT_EQ(ctx.epochMap.find(&target), ctx.epochMap.end());
    // flag still set (not consumed by non-matching call)
    EXPECT_EQ(ctx.skipEpochRecordFor, &target);
}

TEST_F(SkipEpochRecordTest, EmptyEpoch_NoEntryRegardless)
{
    DependencyTracker tracker(pools);
    Value v;
    v.mkInt(42);
    uint32_t epochStart = DependencyTracker::sessionTraces.size();
    // No deps recorded between epochStart and now

    ctx.recordThunkDeps(v, epochStart);

    // epochStart == epochEnd → no entry (not a skip, just nothing to record)
    EXPECT_EQ(ctx.epochMap.find(&v), ctx.epochMap.end());
}

// ═════════════════════════════════════════════════════════════════════
// Component 3: ChildRangeExcluder RAII pattern
// ═════════════════════════════════════════════════════════════════════

// ── Positive: RAII guard excludes on destruction ─────────────────────

TEST_F(ChildRangeExclusionTest, RAII_ExcludesChildDepsOnDestruction)
{
    DependencyTracker parent(pools);
    DependencyTracker::record(makeContentDep(pools,"/p1.nix", "p1"));

    {
        auto * pt = DependencyTracker::activeTracker;
        uint32_t rs = DependencyTracker::sessionTraces.size();

        DependencyTracker::record(makeContentDep(pools,"/child.nix", "c"));

        struct Guard {
            DependencyTracker * t; uint32_t s;
            ~Guard() { if (t) t->excludeChildRange(s, DependencyTracker::sessionTraces.size()); }
        } g{pt, rs};
    }

    DependencyTracker::record(makeContentDep(pools,"/p2.nix", "p2"));
    EXPECT_EQ(keys(pools, parent.collectTraces()), (std::vector<std::string>{"/p1.nix", "/p2.nix"}));
}

TEST_F(ChildRangeExclusionTest, RAII_ExceptionSafety)
{
    DependencyTracker parent(pools);
    DependencyTracker::record(makeContentDep(pools,"/p.nix", "p"));

    try {
        auto * pt = DependencyTracker::activeTracker;
        uint32_t rs = DependencyTracker::sessionTraces.size();
        struct Guard {
            DependencyTracker * t; uint32_t s;
            ~Guard() { if (t) t->excludeChildRange(s, DependencyTracker::sessionTraces.size()); }
        } g{pt, rs};

        DependencyTracker::record(makeContentDep(pools,"/child.nix", "c"));
        throw std::runtime_error("boom");
    } catch (...) {}

    EXPECT_EQ(keys(pools, parent.collectTraces()), (std::vector<std::string>{"/p.nix"}));
}

TEST_F(ChildRangeExclusionTest, RAII_NestedChildrenCompose)
{
    DependencyTracker gp(pools);
    DependencyTracker::record(makeContentDep(pools,"/gp.nix", "gp"));

    {
        auto * gpT = DependencyTracker::activeTracker;
        uint32_t cs = DependencyTracker::sessionTraces.size();

        DependencyTracker child(pools);
        DependencyTracker::record(makeContentDep(pools,"/child.nix", "c"));

        {
            auto * childT = DependencyTracker::activeTracker;
            uint32_t gcs = DependencyTracker::sessionTraces.size();
            DependencyTracker::record(makeContentDep(pools,"/gc.nix", "gc"));
            childT->excludeChildRange(gcs, DependencyTracker::sessionTraces.size());
        }

        // Child trace: only /child.nix (grandchild excluded)
        EXPECT_EQ(keys(pools, child.collectTraces()), (std::vector<std::string>{"/child.nix"}));

        gpT->excludeChildRange(cs, DependencyTracker::sessionTraces.size());
    }

    DependencyTracker::record(makeContentDep(pools,"/gp2.nix", "gp2"));

    // Grandparent trace: only its own deps (child+grandchild excluded)
    EXPECT_EQ(keys(pools, gp.collectTraces()), (std::vector<std::string>{"/gp.nix", "/gp2.nix"}));
}

// ── Negative: null parent = no exclusion ─────────────────────────────

TEST_F(ChildRangeExclusionTest, RAII_NullParent_NothingExcluded)
{
    DependencyTracker tracker(pools);
    DependencyTracker::record(makeContentDep(pools,"/a.nix", "a"));

    {
        struct Guard {
            DependencyTracker * t; uint32_t s;
            ~Guard() { if (t) t->excludeChildRange(s, DependencyTracker::sessionTraces.size()); }
        } g{nullptr, 0}; // null parent → no-op

        DependencyTracker::record(makeContentDep(pools,"/b.nix", "b"));
    }

    DependencyTracker::record(makeContentDep(pools,"/c.nix", "c"));
    EXPECT_EQ(keys(pools, tracker.collectTraces()),
        (std::vector<std::string>{"/a.nix", "/b.nix", "/c.nix"}));
}

} // namespace nix::eval_trace
