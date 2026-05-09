/**
 * Tests for recordThunkDeps and replayMemoizedDeps: verifies that epoch entries
 * are recorded when deps exist, skipped when the epoch range is empty, replayed
 * into later scopes, and that sibling-detection via valueIdentityMap + an active
 * SiblingAccessTracker works correctly.
 */
#include "eval-trace/helpers.hh"
#include "eval-trace/trace-runtime-test-access.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/dep-recording-context.hh"
#include "nix/expr/eval-trace/deps/dep-capture-scope.hh"
#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/value.hh"

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

// ═════════════════════════════════════════════════════════════════════
// Component 2: recordThunkDeps + replayMemoizedDeps epoch behavior
//
// skipEpochRecordFor was removed: TracedExpr thunks now get epoch
// entries like all other thunks. Sibling detection in replayMemoizedDeps
// (via valueIdentityMap + active SiblingAccessTracker) handles the case where
// a sibling's epoch entry would cause dep contamination.
// ═════════════════════════════════════════════════════════════════════

class EpochRecordTest : public ::testing::Test
{
protected:
    InterningPools pools;
    TraceRuntime ctx;
    void SetUp() override {
        eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx).clear();
        eval_trace::test::TraceRuntimeTestAccess::clearReplayEntries(ctx);
    }
    void TearDown() override {
        eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx).clear();
        eval_trace::test::TraceRuntimeTestAccess::clearReplayEntries(ctx);
    }
};

// ── recordThunkDeps always records when deps exist ───────────────────

TEST_F(EpochRecordTest, Record_SingleValue_EntryCreated)
{
    DepRecordingContext dctx(pools, eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
    eval_trace::StandaloneDepCtxGuard guard(dctx);
    Value v;
    v.mkInt(42);
    uint32_t epochStart = ctx.currentReplayEpochSize();
    dctx.record(makeContentDep(pools, "/a.nix", "a"));

    eval_trace::test::TraceRuntimeTestAccess::recordThunkDeps(ctx, v, epochStart);

    auto range = eval_trace::test::TraceRuntimeTestAccess::lookupReplayRange(ctx, v);
    ASSERT_TRUE(range.has_value());
    EXPECT_EQ(range->start, epochStart);
    EXPECT_EQ(range->end, ctx.currentReplayEpochSize());
}

TEST_F(EpochRecordTest, Record_MultipleValues_AllEntriesCreated)
{
    DepRecordingContext dctx(pools, eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
    eval_trace::StandaloneDepCtxGuard guard(dctx);
    Value v1, v2;
    v1.mkInt(1);
    v2.mkInt(2);

    uint32_t e1 = ctx.currentReplayEpochSize();
    dctx.record(makeContentDep(pools, "/a.nix", "a"));
    eval_trace::test::TraceRuntimeTestAccess::recordThunkDeps(ctx, v1, e1);

    uint32_t e2 = ctx.currentReplayEpochSize();
    dctx.record(makeContentDep(pools, "/b.nix", "b"));
    eval_trace::test::TraceRuntimeTestAccess::recordThunkDeps(ctx, v2, e2);

    // Both values get epoch entries
    auto range1 = eval_trace::test::TraceRuntimeTestAccess::lookupReplayRange(ctx, v1);
    auto range2 = eval_trace::test::TraceRuntimeTestAccess::lookupReplayRange(ctx, v2);
    ASSERT_TRUE(range1.has_value());
    ASSERT_TRUE(range2.has_value());
    EXPECT_EQ(range1->start, e1);
    EXPECT_EQ(range2->start, e2);
}

TEST_F(EpochRecordTest, Record_EmptyEpoch_NoMapEntry)
{
    DepRecordingContext dctx(pools, eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
    eval_trace::StandaloneDepCtxGuard guard(dctx);
    Value v;
    v.mkInt(42);
    uint32_t epochStart = ctx.currentReplayEpochSize();
    // No deps recorded between epochStart and now

    eval_trace::test::TraceRuntimeTestAccess::recordThunkDeps(ctx, v, epochStart);

    // epochStart == epochEnd → no entry (nothing to record)
    EXPECT_FALSE(eval_trace::test::TraceRuntimeTestAccess::lookupReplayRange(ctx, v).has_value());
}

TEST_F(EpochRecordTest, Reset_ClearsEpochState_EmptyLog)
{
    DepRecordingContext dctx(pools, eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
    eval_trace::StandaloneDepCtxGuard guard(dctx);
    Value v;
    v.mkInt(42);
    uint32_t epochStart = ctx.currentReplayEpochSize();
    dctx.record(makeContentDep(pools, "/a.nix", "a"));
    eval_trace::test::TraceRuntimeTestAccess::recordThunkDeps(ctx, v, epochStart);
    ASSERT_TRUE(eval_trace::test::TraceRuntimeTestAccess::hasReplayEntries(ctx));

    eval_trace::test::TraceRuntimeTestAccess::reset(ctx);

    EXPECT_FALSE(eval_trace::test::TraceRuntimeTestAccess::hasReplayEntries(ctx));
}

// ── replayMemoizedDeps copies deps from epoch range ──────────────────

TEST_F(EpochRecordTest, EpochReplay_MemoizedDeps_CopiesDeps)
{
    // Record deps for a value in an outer context
    DepRecordingContext dctx(pools, eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
    eval_trace::StandaloneDepCtxGuard guard(dctx);
    Value v;
    v.mkInt(42);
    uint32_t epochStart = ctx.currentReplayEpochSize();
    dctx.record(makeContentDep(pools, "/a.nix", "a"));
    eval_trace::test::TraceRuntimeTestAccess::recordThunkDeps(ctx, v, epochStart);

    auto outerDeps = TestScopeAccess::takeDeps(dctx);

    // Push a new scope and replay — deps should be copied
    TestScopeAccess::pushScope(dctx);
    eval_trace::test::TraceRuntimeTestAccess::replayMemoizedDeps(ctx, v);

    auto innerDeps = TestScopeAccess::takeDeps(dctx);
    TestScopeAccess::popScope(dctx);
    EXPECT_EQ(innerDeps.size(), 1u);
    EXPECT_EQ(keys(pools, innerDeps), (std::vector<std::string>{"/a.nix"}));
}

TEST_F(EpochRecordTest, EpochReplay_MemoizedDeps_SkipsOwnEpoch)
{
    // If the epoch range was recorded during this scope's lifetime,
    // replay should skip (deps already in ownDeps).
    DepRecordingContext dctx(pools, eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
    eval_trace::StandaloneDepCtxGuard guard(dctx);
    Value v;
    v.mkInt(42);
    uint32_t epochStart = ctx.currentReplayEpochSize();
    dctx.record(makeContentDep(pools, "/a.nix", "a"));
    eval_trace::test::TraceRuntimeTestAccess::recordThunkDeps(ctx, v, epochStart);

    // Replay within same scope — range.start >= epochLogStartIndex
    eval_trace::test::TraceRuntimeTestAccess::replayMemoizedDeps(ctx, v);

    // Deps should only appear once (from the direct record, not replay)
    auto deps = TestScopeAccess::takeDeps(dctx);
    EXPECT_EQ(deps.size(), 1u);
}

// ── replayMemoizedDeps sibling detection via replay-capture hook ─────────────
//
// valueIdentityMap plus an active SiblingAccessTracker enable detection of
// already-materialized siblings in replayMemoizedDeps. Replay skipping requires
// both sibling registration and an active tracker; a bare value-identity
// registration is not enough.

TEST_F(EpochRecordTest, EpochReplay_WithoutTracker_FallsThrough)
{
    // Record deps for a value in an outer context, then collect to end it.
    DepRecordingContext dctx(pools, eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
    eval_trace::StandaloneDepCtxGuard guard(dctx);
    Value v;
    v.mkInt(42);
    uint32_t epochStart = ctx.currentReplayEpochSize();
    dctx.record(makeContentDep(pools, "/a.nix", "a"));
    eval_trace::test::TraceRuntimeTestAccess::recordThunkDeps(ctx, v, epochStart);
    auto outerDeps = TestScopeAccess::takeDeps(dctx);

    // Register v for sibling detection. Without an active
    // SiblingAccessTracker, replay must still fall through to normal dep
    // copying.
    eval_trace::test::TraceRuntimeTestAccess::registerValueIdentity(ctx, &v);

    TestScopeAccess::pushScope(dctx);
    eval_trace::test::TraceRuntimeTestAccess::replayMemoizedDeps(ctx, v);

    auto innerDeps = TestScopeAccess::takeDeps(dctx);
    TestScopeAccess::popScope(dctx);
    EXPECT_EQ(innerDeps.size(), 1u)
        << "replay should not be skipped without an active sibling tracker";
    EXPECT_EQ(keys(pools, innerDeps), (std::vector<std::string>{"/a.nix"}));
}

TEST_F(EpochRecordTest, EpochReplay_ValueIdentityWithoutTracker_NormalReplay)
{
    // Record deps in an outer context.
    DepRecordingContext dctx(pools, eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
    eval_trace::StandaloneDepCtxGuard guard(dctx);
    Value v;
    v.mkInt(42);
    uint32_t epochStart = ctx.currentReplayEpochSize();
    dctx.record(makeContentDep(pools, "/a.nix", "a"));
    eval_trace::test::TraceRuntimeTestAccess::recordThunkDeps(ctx, v, epochStart);
    auto outerDeps = TestScopeAccess::takeDeps(dctx);

    // Register in valueIdentityMap but do not create a sibling tracker.
    eval_trace::test::TraceRuntimeTestAccess::registerValueIdentity(ctx, &v);

    TestScopeAccess::pushScope(dctx);
    eval_trace::test::TraceRuntimeTestAccess::replayMemoizedDeps(ctx, v);

    auto innerDeps = TestScopeAccess::takeDeps(dctx);
    TestScopeAccess::popScope(dctx);
    EXPECT_EQ(innerDeps.size(), 1u)
        << "value identity without an active sibling tracker should fall through to normal dep replay";
    EXPECT_EQ(keys(pools, innerDeps), (std::vector<std::string>{"/a.nix"}));
}

TEST_F(EpochRecordTest, EpochReplay_ValueNotInMap_NormalReplay)
{
    // Record deps in an outer context.
    DepRecordingContext dctx(pools, eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
    eval_trace::StandaloneDepCtxGuard guard(dctx);
    Value v;
    v.mkInt(42);
    uint32_t epochStart = ctx.currentReplayEpochSize();
    dctx.record(makeContentDep(pools, "/a.nix", "a"));
    eval_trace::test::TraceRuntimeTestAccess::recordThunkDeps(ctx, v, epochStart);
    auto outerDeps = TestScopeAccess::takeDeps(dctx);

    // Do not register v in valueIdentityMap.
    ASSERT_FALSE(eval_trace::test::TraceRuntimeTestAccess::hasValueIdentity(ctx, &v));

    TestScopeAccess::pushScope(dctx);
    eval_trace::test::TraceRuntimeTestAccess::replayMemoizedDeps(ctx, v);

    auto innerDeps = TestScopeAccess::takeDeps(dctx);
    TestScopeAccess::popScope(dctx);
    EXPECT_EQ(innerDeps.size(), 1u)
        << "values outside the identity map should replay normally";
    EXPECT_EQ(keys(pools, innerDeps), (std::vector<std::string>{"/a.nix"}));
}

TEST_F(EpochRecordTest, Reset_ClearsValueIdentityState_EmptyMap)
{
    Value v;
    v.mkInt(42);
    eval_trace::test::TraceRuntimeTestAccess::registerValueIdentity(ctx, &v);

    eval_trace::test::TraceRuntimeTestAccess::reset(ctx);

    EXPECT_FALSE(eval_trace::test::TraceRuntimeTestAccess::hasValueIdentity(ctx, &v));
}

} // namespace nix::eval_trace
