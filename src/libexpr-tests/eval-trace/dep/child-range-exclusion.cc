#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/dep-recording-context.hh"
#include "nix/expr/eval-trace/deps/dep-capture-scope.hh"
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
        out.push_back(depKeyDisplayForTest(pools, d));
    return out;
}

class ChildRangeExclusionTest : public ::testing::Test
{
protected:
    InterningPools pools;
    std::vector<Dep> epochLog;
    void SetUp() override { epochLog.clear(); }
    void TearDown() override { epochLog.clear(); }
};

// ═════════════════════════════════════════════════════════════════════
// Component 1: Structural isolation tests (per-tracker ownDeps)
// ═════════════════════════════════════════════════════════════════════

// ── Positive: inner tracker collects only inner deps ─────────────────

TEST_F(ChildRangeExclusionTest, ScopeIsolation_NestedTracker_InnerOnlyHasInnerDeps)
{
    DepRecordingContext ctx(pools, epochLog);
        TestScopeAccess::pushScope(ctx);
    ctx.record(makeContentDep(pools, "/outer1.nix", "o1"));

    std::vector<Dep> innerDeps;
    {
        TestScopeAccess::pushScope(ctx);
        ctx.record(makeContentDep(pools, "/inner1.nix", "i1"));
        ctx.record(makeContentDep(pools, "/inner2.nix", "i2"));
        innerDeps = TestScopeAccess::takeDeps(ctx);
        TestScopeAccess::popScope(ctx);
    }

    ctx.record(makeContentDep(pools, "/outer2.nix", "o2"));

    EXPECT_EQ(keys(pools, innerDeps), (std::vector<std::string>{"/inner1.nix", "/inner2.nix"}));
}

TEST_F(ChildRangeExclusionTest, ScopeIsolation_NestedTracker_OuterOnlyHasOuterDeps)
{
    DepRecordingContext ctx(pools, epochLog);
        TestScopeAccess::pushScope(ctx);
    ctx.record(makeContentDep(pools, "/outer1.nix", "o1"));

    {
        TestScopeAccess::pushScope(ctx);
        ctx.record(makeContentDep(pools, "/inner1.nix", "i1"));
        ctx.record(makeContentDep(pools, "/inner2.nix", "i2"));
        // inner scope popped here; inner deps stay in inner's ownDeps
        TestScopeAccess::popScope(ctx);
    }

    ctx.record(makeContentDep(pools, "/outer2.nix", "o2"));

    auto outerDeps = TestScopeAccess::takeDeps(ctx);
    EXPECT_EQ(keys(pools, outerDeps), (std::vector<std::string>{"/outer1.nix", "/outer2.nix"}));
}

TEST_F(ChildRangeExclusionTest, ScopeIsolation_MultipleChildren_EachCollectsOwnDeps)
{
    DepRecordingContext ctx(pools, epochLog);
        TestScopeAccess::pushScope(ctx);
    ctx.record(makeContentDep(pools, "/p1.nix", "p1"));

    std::vector<Dep> child1Deps, child2Deps;
    {
        TestScopeAccess::pushScope(ctx);
        ctx.record(makeContentDep(pools, "/c1a.nix", "c1a"));
        ctx.record(makeContentDep(pools, "/c1b.nix", "c1b"));
        child1Deps = TestScopeAccess::takeDeps(ctx);
        TestScopeAccess::popScope(ctx);
    }

    ctx.record(makeContentDep(pools, "/p2.nix", "p2"));

    {
        TestScopeAccess::pushScope(ctx);
        ctx.record(makeContentDep(pools, "/c2a.nix", "c2a"));
        child2Deps = TestScopeAccess::takeDeps(ctx);
        TestScopeAccess::popScope(ctx);
    }

    ctx.record(makeContentDep(pools, "/p3.nix", "p3"));

    EXPECT_EQ(keys(pools, child1Deps), (std::vector<std::string>{"/c1a.nix", "/c1b.nix"}));
    EXPECT_EQ(keys(pools, child2Deps), (std::vector<std::string>{"/c2a.nix"}));

    auto parentDeps = TestScopeAccess::takeDeps(ctx);
    EXPECT_EQ(keys(pools, parentDeps), (std::vector<std::string>{"/p1.nix", "/p2.nix", "/p3.nix"}));
}

TEST_F(ChildRangeExclusionTest, ScopeIsolation_EmptyTracker_YieldsEmpty)
{
    DepRecordingContext ctx(pools, epochLog);
        TestScopeAccess::pushScope(ctx);
    // No deps recorded
    EXPECT_TRUE(TestScopeAccess::takeDeps(ctx).empty());
}

TEST_F(ChildRangeExclusionTest, ScopeIsolation_NoExclusionNeeded_ParentDepsAutoClean)
{
    DepRecordingContext ctx(pools, epochLog);
        TestScopeAccess::pushScope(ctx);
    ctx.record(makeContentDep(pools, "/a.nix", "a"));

    {
        TestScopeAccess::pushScope(ctx);
        ctx.record(makeContentDep(pools, "/child.nix", "c"));
        TestScopeAccess::popScope(ctx);
    }

    ctx.record(makeContentDep(pools, "/b.nix", "b"));

    // Parent never called excludeChildRange, yet child deps are absent
    auto parentDeps = TestScopeAccess::takeDeps(ctx);
    EXPECT_EQ(keys(pools, parentDeps), (std::vector<std::string>{"/a.nix", "/b.nix"}));
}

TEST_F(ChildRangeExclusionTest, ScopeIsolation_DepsBetweenChildren_GoToOuter)
{
    DepRecordingContext ctx(pools, epochLog);
        TestScopeAccess::pushScope(ctx);
    ctx.record(makeContentDep(pools, "/before.nix", "b"));

    {
        TestScopeAccess::pushScope(ctx);
        ctx.record(makeContentDep(pools, "/c1.nix", "c1"));
        TestScopeAccess::popScope(ctx);
    }

    // This dep is recorded after child scope is popped, so currentScope
    // is back to outer — it goes into outer's ownDeps.
    ctx.record(makeContentDep(pools, "/between.nix", "mid"));

    {
        TestScopeAccess::pushScope(ctx);
        ctx.record(makeContentDep(pools, "/c2.nix", "c2"));
        TestScopeAccess::popScope(ctx);
    }

    ctx.record(makeContentDep(pools, "/after.nix", "a"));

    auto outerDeps = TestScopeAccess::takeDeps(ctx);
    EXPECT_EQ(keys(pools, outerDeps),
        (std::vector<std::string>{"/before.nix", "/between.nix", "/after.nix"}));
}

// ── Negative: tracker with all deps excluded is empty ────────────────

TEST_F(ChildRangeExclusionTest, ScopeIsolation_AllDepsInChild_ParentEmpty)
{
    DepRecordingContext ctx(pools, epochLog);
        TestScopeAccess::pushScope(ctx);

    {
        TestScopeAccess::pushScope(ctx);
        ctx.record(makeContentDep(pools, "/only.nix", "only"));
        TestScopeAccess::popScope(ctx);
    }

    // Parent recorded nothing itself
    EXPECT_TRUE(TestScopeAccess::takeDeps(ctx).empty());
}

TEST_F(ChildRangeExclusionTest, ScopeIsolation_SingleTrackerNoDeps_AllDepsPreserved)
{
    DepRecordingContext ctx(pools, epochLog);
        TestScopeAccess::pushScope(ctx);
    ctx.record(makeContentDep(pools, "/a.nix", "a"));
    ctx.record(makeContentDep(pools, "/b.nix", "b"));
    ctx.record(makeContentDep(pools, "/c.nix", "c"));

    // No nested scopes, no exclusions — all deps preserved
    EXPECT_EQ(keys(pools, TestScopeAccess::takeDeps(ctx)),
        (std::vector<std::string>{"/a.nix", "/b.nix", "/c.nix"}));
}

// ═════════════════════════════════════════════════════════════════════
// Component 3: Structural isolation via nested DepRecordingContext scopes
// ═════════════════════════════════════════════════════════════════════

// ── Positive: nested scope provides automatic isolation ────

TEST_F(ChildRangeExclusionTest, ScopeIsolation_RAII_InnerDepsOnlyInInner)
{
    DepRecordingContext ctx(pools, epochLog);
        TestScopeAccess::pushScope(ctx);
    ctx.record(makeContentDep(pools, "/p1.nix", "p1"));

    {
        TestScopeAccess::pushScope(ctx);
        ctx.record(makeContentDep(pools, "/child.nix", "c"));
        // child scope popped here; its deps are structurally isolated
        TestScopeAccess::popScope(ctx);
    }

    ctx.record(makeContentDep(pools, "/p2.nix", "p2"));
    EXPECT_EQ(keys(pools, TestScopeAccess::takeDeps(ctx)), (std::vector<std::string>{"/p1.nix", "/p2.nix"}));
}

TEST_F(ChildRangeExclusionTest, ScopeIsolation_RAII_ExceptionLeavesParentClean)
{
    DepRecordingContext ctx(pools, epochLog);
        TestScopeAccess::pushScope(ctx);
    ctx.record(makeContentDep(pools, "/p.nix", "p"));

    try {
        TestScopeAccess::pushScope(ctx);
        ctx.record(makeContentDep(pools, "/child.nix", "c"));
        throw std::runtime_error("boom");
        // child scope NOT popped via exception; manually pop in catch
    } catch (...) {
        TestScopeAccess::popScope(ctx);
    }

    // Parent is unaffected — child's deps were in child's own scope
    EXPECT_EQ(keys(pools, TestScopeAccess::takeDeps(ctx)), (std::vector<std::string>{"/p.nix"}));
}

TEST_F(ChildRangeExclusionTest, ScopeIsolation_RAII_NestedChildrenCompose)
{
    DepRecordingContext ctx(pools, epochLog);
        TestScopeAccess::pushScope(ctx);
    ctx.record(makeContentDep(pools, "/gp.nix", "gp"));

    std::vector<Dep> childDeps;
    {
        TestScopeAccess::pushScope(ctx);
        ctx.record(makeContentDep(pools, "/child.nix", "c"));

        {
            TestScopeAccess::pushScope(ctx);
            ctx.record(makeContentDep(pools, "/gc.nix", "gc"));
            // grandchild collects only /gc.nix
            EXPECT_EQ(keys(pools, TestScopeAccess::takeDeps(ctx)), (std::vector<std::string>{"/gc.nix"}));
            TestScopeAccess::popScope(ctx);
        }

        // child collects only /child.nix (grandchild's deps are in grandchild's scope)
        childDeps = TestScopeAccess::takeDeps(ctx);
        EXPECT_EQ(keys(pools, childDeps), (std::vector<std::string>{"/child.nix"}));
        TestScopeAccess::popScope(ctx);
    }

    ctx.record(makeContentDep(pools, "/gp2.nix", "gp2"));

    // Grandparent collects only its own deps
    EXPECT_EQ(keys(pools, TestScopeAccess::takeDeps(ctx)),
        (std::vector<std::string>{"/gp.nix", "/gp2.nix"}));
}

// ── Negative: no nested scope = all deps go to current scope ─────

TEST_F(ChildRangeExclusionTest, ScopeIsolation_RAII_NoNestedTracker_AllDepsInCurrent)
{
    DepRecordingContext ctx(pools, epochLog);
        TestScopeAccess::pushScope(ctx);
    ctx.record(makeContentDep(pools, "/a.nix", "a"));
    ctx.record(makeContentDep(pools, "/b.nix", "b"));
    ctx.record(makeContentDep(pools, "/c.nix", "c"));

    EXPECT_EQ(keys(pools, TestScopeAccess::takeDeps(ctx)),
        (std::vector<std::string>{"/a.nix", "/b.nix", "/c.nix"}));
}

} // namespace nix::eval_trace
