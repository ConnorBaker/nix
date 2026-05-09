#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/dep-recording-context.hh"
#include "nix/expr/eval-trace/deps/input-resolution.hh"
#include "nix/expr/eval-trace/deps/shape-recording.hh"
#include "nix/expr/eval-trace/deps/trace-frame.hh"
#include "nix/expr/eval-trace/deps/trace-access.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/deps/replay-publish-scope.hh"
#include "nix/util/source-path.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DepRecordingContextTest : public ::testing::Test
{
protected:
    InterningPools pools;
    std::vector<Dep> epochLog;

    void SetUp() override
    {
        epochLog.clear();
    }

    void TearDown() override
    {
        epochLog.clear();
    }
};

// ── RAII dependency context tests ────────────────────────────────────

TEST_F(DepRecordingContextTest, Scope_PushPop_DepthChanges)
{
    DepRecordingContext ctx(pools, epochLog);
    EXPECT_FALSE(ctx.isActive()); // no scope until DepCaptureScope pushes
    EXPECT_EQ(ctx.depth(), 0u);

    TestScopeAccess::pushScope(ctx);
    EXPECT_TRUE(ctx.isActive());
    EXPECT_EQ(ctx.depth(), 1u);

    TestScopeAccess::popScope(ctx);
    EXPECT_EQ(ctx.depth(), 0u);
}

TEST_F(DepRecordingContextTest, Record_WithActiveScope_DepsCaptured)
{
    DepRecordingContext ctx(pools, epochLog);
    TestScopeAccess::pushScope(ctx);
    auto dep = makeContentDep(pools, "/test.nix", "content");
    ctx.record(dep);
    auto deps = TestScopeAccess::takeDeps(ctx);
    ASSERT_EQ(deps.size(), 1u);
    EXPECT_EQ(depKeyDisplayForTest(pools, deps[0]), "/test.nix");
    EXPECT_EQ(deps[0].key.kind, CanonicalQueryKind::FileBytes);
}

TEST_F(DepRecordingContextTest, CollectDeps_OnlyCurrentScope_NoLeakToParent)
{
    DepRecordingContext ctx(pools, epochLog);
    TestScopeAccess::pushScope(ctx);
    ctx.record(makeContentDep(pools, "/outer.nix", "outer"));
    TestScopeAccess::pushScope(ctx);
    ctx.record(makeContentDep(pools, "/inner.nix", "inner"));
    auto innerDeps = TestScopeAccess::takeDeps(ctx);
    ASSERT_EQ(innerDeps.size(), 1u);
    EXPECT_EQ(depKeyDisplayForTest(pools, innerDeps[0]), "/inner.nix");
    TestScopeAccess::popScope(ctx);
    auto outerDeps = TestScopeAccess::takeDeps(ctx);
    EXPECT_EQ(outerDeps.size(), 1u);
    EXPECT_EQ(depKeyDisplayForTest(pools, outerDeps[0]), "/outer.nix");
}

TEST_F(DepRecordingContextTest, Dedup_ExactDuplicates_Collapse)
{
    DepRecordingContext ctx(pools, epochLog);
    TestScopeAccess::pushScope(ctx);
    auto dep = makeContentDep(pools, "/same.nix", "content");
    ctx.record(dep);
    ctx.record(dep);
    auto deps = TestScopeAccess::takeDeps(ctx);
    EXPECT_EQ(deps.size(), 1u);
}

TEST_F(DepRecordingContextTest, Dedup_Conflict_MarksUnstable)
{
    DepRecordingContext ctx(pools, epochLog);
    TestScopeAccess::pushScope(ctx);
    auto dep1 = makeContentDep(pools, "/test.nix", "hash1");
    auto dep2 = makeContentDep(pools, "/test.nix", "hash2");
    ctx.record(dep1);
    ctx.record(dep2);
    EXPECT_FALSE(ctx.isStable());
}

} // namespace nix::eval_trace
