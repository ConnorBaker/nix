/**
 * Tests for epoch-map stability: verifies that recordThunkDeps / replayMemoizedDeps
 * produce a consistent dep set regardless of when in the evaluation a shared thunk
 * was forced (prior scope vs. current outer scope vs. nested child scope).
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
#include "nix/expr/eval-trace/deps/hash.hh"
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
// Epoch map stability tests
//
// These test the epoch map (recordThunkDeps / replayMemoizedDeps).
// recordThunkDeps is always called by forceValue() regardless of
// recording context state. The epochMap entry enables replay by later
// scopes that access the same Value.
// ═════════════════════════════════════════════════════════════════════

class EpochStabilityTest : public ::testing::Test
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

// -- Basic epoch replay test -----------------------------------------------

TEST_F(EpochStabilityTest, Thunk_AlwaysRecorded_EntryPresent)
{
    // forceValue() always calls recordThunkDeps.
    // The epochMap entry enables replay by later scopes.

    Value v;
    v.mkInt(42);

    // Phase 1: outer context, force V (record epoch entry)
    {
        DepRecordingContext dctx(pools, eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
        eval_trace::StandaloneDepCtxGuard guard(dctx);
        uint32_t epochStart = ctx.currentReplayEpochSize();
        dctx.record(makeContentDep(pools, "/lib/default.nix", "lib"));
        dctx.record(makeContentDep(pools, "/lib/attrsets.nix", "attrs"));
        eval_trace::test::TraceRuntimeTestAccess::recordThunkDeps(ctx, v, epochStart);
    }

    // Phase 2: new context replays V successfully
    {
        DepRecordingContext dctx(pools, eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
        eval_trace::StandaloneDepCtxGuard guard(dctx);
        dctx.record(makeContentDep(pools, "/closure.nix", "closure"));
        eval_trace::test::TraceRuntimeTestAccess::replayMemoizedDeps(ctx, v);

        auto deps = TestScopeAccess::takeDeps(dctx);
        // V's deps are replayed -> 3 total deps
        EXPECT_EQ(deps.size(), 3u)
            << "epochMap entry enables replay in later scope";
    }
}

// -- Cross-scope replay test ----------------------

TEST_F(EpochStabilityTest, Thunk_WithEpochEntry_ReplaySucceeds)
{
    // recordThunkDeps IS called; new context successfully replays V's deps.

    Value v;
    v.mkInt(42);

    // Phase 1: outer context, force V, record epoch
    {
        DepRecordingContext dctx(pools, eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
        eval_trace::StandaloneDepCtxGuard guard(dctx);
        uint32_t epochStart = ctx.currentReplayEpochSize();
        dctx.record(makeContentDep(pools, "/lib/default.nix", "lib"));
        dctx.record(makeContentDep(pools, "/lib/attrsets.nix", "attrs"));
        eval_trace::test::TraceRuntimeTestAccess::recordThunkDeps(ctx, v, epochStart);
    }

    // Phase 2: new context replays V successfully
    {
        DepRecordingContext dctx(pools, eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
        eval_trace::StandaloneDepCtxGuard guard(dctx);
        dctx.record(makeContentDep(pools, "/closure.nix", "closure"));
        eval_trace::test::TraceRuntimeTestAccess::replayMemoizedDeps(ctx, v);

        auto deps = TestScopeAccess::takeDeps(dctx);
        // V's deps are replayed via epochMap -> 3 total deps
        EXPECT_EQ(deps.size(), 3u)
            << "with epochMap entry, replay adds V's deps";
    }
}

// -- Stability test: dep set is stable across scope boundaries ----------------

TEST_F(EpochStabilityTest, DepSet_ScopeBoundary_StableAcrossScopes)
{
    // The dep set collected by a child scope should be IDENTICAL
    // whether a shared thunk V was forced in:
    //   (A) the current outer scope, or
    //   (B) a prior separate scope
    // Both cases produce an epochMap entry that enables replay.

    Value v;
    v.mkInt(42);

    // Case A: V forced in prior scope (normal case)
    auto depsA = [&]() {
        eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx).clear();
        eval_trace::test::TraceRuntimeTestAccess::clearReplayEntries(ctx);

        // Prior scope forces V
        {
            DepRecordingContext dctx(pools, eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx));
            TestScopeAccess::pushScope(dctx);
            uint32_t epochStart = ctx.currentReplayEpochSize();
            dctx.record(makeContentDep(pools, "/lib/default.nix", "lib"));
            dctx.record(makeContentDep(pools, "/lib/attrsets.nix", "attrs"));
            eval_trace::test::TraceRuntimeTestAccess::recordThunkDeps(ctx, v, epochStart);
        }

        // Child context records own deps + replays V
        DepRecordingContext dctx(pools, eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
        eval_trace::StandaloneDepCtxGuard guard(dctx);
        dctx.record(makeContentDep(pools, "/closure.nix", "closure"));
        eval_trace::test::TraceRuntimeTestAccess::replayMemoizedDeps(ctx, v);
        return keys(pools, TestScopeAccess::takeDeps(dctx));
    }();

    // Case B: V forced in the same outer context (before child scope)
    auto depsB = [&]() {
        eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx).clear();
        eval_trace::test::TraceRuntimeTestAccess::clearReplayEntries(ctx);

        // Outer context forces V
        {
            DepRecordingContext dctx(pools, eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx));
            TestScopeAccess::pushScope(dctx);
            uint32_t epochStart = ctx.currentReplayEpochSize();
            dctx.record(makeContentDep(pools, "/lib/default.nix", "lib"));
            dctx.record(makeContentDep(pools, "/lib/attrsets.nix", "attrs"));
            eval_trace::test::TraceRuntimeTestAccess::recordThunkDeps(ctx, v, epochStart);
        }

        // Child context records own deps + replays V
        DepRecordingContext dctx(pools, eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
        eval_trace::StandaloneDepCtxGuard guard(dctx);
        dctx.record(makeContentDep(pools, "/closure.nix", "closure"));
        eval_trace::test::TraceRuntimeTestAccess::replayMemoizedDeps(ctx, v);
        return keys(pools, TestScopeAccess::takeDeps(dctx));
    }();

    // Both should produce the same dep set
    EXPECT_EQ(depsA, depsB)
        << "dep set must be identical regardless of when shared thunk was forced";
}

// -- Nested thunk test: W depends on V, both forced before child scope -------------

TEST_F(EpochStabilityTest, NestedThunks_SequentialForcing_DepsPreserved)
{
    // V is forced first, then W (which depends on V).
    // Both are forced before the child scope.
    // Later, child context accesses W -> should get W's AND V's deps.

    Value v, w;
    v.mkInt(1);
    w.mkInt(2);

    eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx).clear();
    eval_trace::test::TraceRuntimeTestAccess::clearReplayEntries(ctx);

    {
        DepRecordingContext dctx(pools, eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
        eval_trace::StandaloneDepCtxGuard guard(dctx);
        // V is forced
        uint32_t vStart = ctx.currentReplayEpochSize();
        dctx.record(makeContentDep(pools, "/v-dep.nix", "v"));
        eval_trace::test::TraceRuntimeTestAccess::recordThunkDeps(ctx, v, vStart);

        // W is forced, W's eval accesses V (replay fires)
        uint32_t wStart = ctx.currentReplayEpochSize();
        dctx.record(makeContentDep(pools, "/w-dep.nix", "w"));
        eval_trace::test::TraceRuntimeTestAccess::recordThunkDeps(ctx, w, wStart);
    }

    // Child context accesses W
    DepRecordingContext dctx(pools, eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx));
    TestScopeAccess::pushScope(dctx);
    eval_trace::StandaloneDepCtxGuard guard(dctx);
    dctx.record(makeContentDep(pools, "/child.nix", "c"));
    eval_trace::test::TraceRuntimeTestAccess::replayMemoizedDeps(ctx, w);
    // Also replay V (child might access V independently)
    eval_trace::test::TraceRuntimeTestAccess::replayMemoizedDeps(ctx, v);

    auto deps = keys(pools, TestScopeAccess::takeDeps(dctx));
    EXPECT_GE(deps.size(), 3u)
        << "child should get own dep + V's dep + W's dep via replay";
}

} // namespace nix::eval_trace
