// epoch-bugs.cc — Unit tests for eval-trace epoch memoization and scope lifecycle.
//
// Covers the bugs fixed in the epoch memoization and DepCaptureScope lifecycle:
//
//   BUG-7 (MEDIUM): recordThunkDeps used emplace (no-op on duplicate key).
//     Fixed by using insert_or_assign so GC-recycled Value* addresses get
//     fresh DepRange entries instead of retaining stale ones.
//
//   Theme 11+12: takeDeps() → finalizeAndTakeDeps(), which also pops the scope.
//     After the call the scope is gone; further recording goes to the parent.
//     The destructor checks scopePopped and skips the pop to prevent double-pop.
//
//   BUG-3 (HIGH): mergeIntoParent called rollbackReplayEpoch, erasing epochMap
//     entries for sub-thunks forced during a warmup scope.  The fix removes the
//     rollback.  Test 5 is a TraceCacheFixture integration test that exercises
//     the PublicationWarmupScope → mergeIntoParent path via EvalState.

#include "eval-trace/helpers.hh"
#include "eval-trace/trace-runtime-test-access.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/dep-recording-context.hh"
#include "nix/expr/eval-trace/deps/dep-capture-scope.hh"
#include "nix/expr/eval-trace/deps/memo-replay-store.hh"
#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/value.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── Local helper: extract dep keys for readable EXPECT assertions ─────

static std::vector<std::string> keys(InterningPools & pools, const std::vector<Dep> & deps)
{
    std::vector<std::string> out;
    out.reserve(deps.size());
    for (auto & d : deps)
        out.push_back(depKeyDisplayForTest(pools, d));
    return out;
}

// ═════════════════════════════════════════════════════════════════════
// Fixture for MemoReplayStore tests (BUG-7)
//
// Uses TraceRuntime directly, mirroring EpochRecordTest in
// child-range-exclusion.cc and dep-stability.cc.
// ═════════════════════════════════════════════════════════════════════

class EpochBugTest : public ::testing::Test
{
protected:
    InterningPools pools;
    TraceRuntime ctx;

    void SetUp() override
    {
        eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx).clear();
        eval_trace::test::TraceRuntimeTestAccess::clearReplayEntries(ctx);
    }

    void TearDown() override
    {
        eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx).clear();
        eval_trace::test::TraceRuntimeTestAccess::clearReplayEntries(ctx);
    }
};

// ═════════════════════════════════════════════════════════════════════
// Test 1: InsertOrAssign_OverwritesStaleEpochMapEntry (BUG-7)
//
// Simulates GC address reuse: a Value* is force-recorded with one
// DepRange, then the same pointer is force-recorded again with a
// different DepRange (as if GC reclaimed the old Value and a new one
// was allocated at the same address).
//
// With the old `emplace` behavior the second insert would be a no-op —
// the stale [0,3) entry would be kept and replaying into a new scope
// would produce deps from the first range, not the second.
//
// With `insert_or_assign` the second record overwrites the entry, so
// replay correctly returns deps from [5,8).
// ═════════════════════════════════════════════════════════════════════

TEST_F(EpochBugTest, Epoch_InsertOrAssign_OverwritesStaleEntry)
{
    // Shared Value* address — simulates a GC-reused address.
    Value v;
    v.mkInt(1);

    // ── First "allocation": record 3 deps [0,3) for &v ──────────────
    {
        DepRecordingContext dctx(pools, eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
        eval_trace::StandaloneDepCtxGuard guard(dctx);

        uint32_t epochStart = ctx.currentReplayEpochSize(); // 0
        dctx.record(makeContentDep(pools, "/old/a.nix", "a0"));
        dctx.record(makeContentDep(pools, "/old/b.nix", "b0"));
        dctx.record(makeContentDep(pools, "/old/c.nix", "c0"));
        eval_trace::test::TraceRuntimeTestAccess::recordThunkDeps(ctx, v, epochStart); // maps &v → [0, 3)

        // epoch is now at size 3; first entry committed
        TestScopeAccess::takeDeps(dctx);
    }

    // Confirm entry exists and points to the first range.
    {
        auto range = eval_trace::test::TraceRuntimeTestAccess::lookupReplayRange(ctx, v);
        ASSERT_TRUE(range.has_value());
        EXPECT_EQ(range->start, 0u);
        EXPECT_EQ(range->end, 3u);
    }

    // ── Append 2 filler deps (indices 3 and 4) so the second range
    //    starts at index 5, making the ranges non-overlapping. ────────
    eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx).push_back(makeContentDep(pools, "/filler/x.nix", "fx"));
    eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx).push_back(makeContentDep(pools, "/filler/y.nix", "fy"));

    // ── Second "allocation": same address, new range [5, 8) ──────────
    {
        DepRecordingContext dctx(pools, eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
        eval_trace::StandaloneDepCtxGuard guard(dctx);

        uint32_t epochStart = ctx.currentReplayEpochSize(); // 5
        dctx.record(makeContentDep(pools, "/new/p.nix", "p1"));
        dctx.record(makeContentDep(pools, "/new/q.nix", "q1"));
        dctx.record(makeContentDep(pools, "/new/r.nix", "r1"));
        eval_trace::test::TraceRuntimeTestAccess::recordThunkDeps(ctx, v, epochStart); // must overwrite → [5, 8)

        TestScopeAccess::takeDeps(dctx);
    }

    // Confirm the entry has been overwritten.
    {
        auto range = eval_trace::test::TraceRuntimeTestAccess::lookupReplayRange(ctx, v);
        ASSERT_TRUE(range.has_value());
        EXPECT_EQ(range->start, 5u)
            << "insert_or_assign must overwrite the stale entry (BUG-7: emplace kept [0,3))";
        EXPECT_EQ(range->end, 8u);
    }

    // Replay into a fresh scope and verify we get the NEW deps, not the old ones.
    {
        DepRecordingContext dctx(pools, eval_trace::test::TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
        eval_trace::StandaloneDepCtxGuard guard(dctx);

        // The scope's epochLogStartIndex is 8 (the epoch log has 8 entries when
        // pushScope fires).  The replay range is [5,8), so range.start (5) <
        // epochLogStartIndex (8) — the replay fires.  We also record an own dep
        // to make the expected count (1 own + 3 replayed = 4) unambiguous.
        dctx.record(makeContentDep(pools, "/scope/own.nix", "own"));

        eval_trace::test::TraceRuntimeTestAccess::replayMemoizedDeps(ctx, v);

        auto deps = TestScopeAccess::takeDeps(dctx);
        auto k = keys(pools, deps);

        // Must contain /new/* deps (from [5,8)), NOT /old/* deps (from [0,3)).
        EXPECT_EQ(k.size(), 4u)
            << "own dep + 3 new deps from range [5,8)";

        bool hasNewP = std::find(k.begin(), k.end(), "/new/p.nix") != k.end();
        bool hasOldA = std::find(k.begin(), k.end(), "/old/a.nix") != k.end();

        EXPECT_TRUE(hasNewP)
            << "replay must return new deps after insert_or_assign overwrote stale entry";
        EXPECT_FALSE(hasOldA)
            << "old deps from stale entry must NOT appear after GC address reuse (BUG-7)";
    }
}

// ═════════════════════════════════════════════════════════════════════
// Test 2: FinalizeAndTakeDeps_PopsScope (Theme 11+12)
//
// finalizeAndTakeDeps() takes the current scope's deps AND pops the
// scope atomically (sets scopePopped = true).  After the call the
// DepRecordingContext should have one fewer scope on its stack, and
// subsequent record() calls go to the parent scope (or nowhere if the
// parent is also gone).
// ═════════════════════════════════════════════════════════════════════

TEST_F(EpochBugTest, Epoch_FinalizeAndTakeDeps_PopsScope)
{
    // Use DepCaptureScope to model the production pattern.
    // Standalone test constructor: no SemanticRegistry needed.
    DepCaptureScope parentScope(pools);

    // The parent scope is now active.  Verify the standalone context depth.
    auto * parentCtx = parentScope.ctx;
    ASSERT_NE(parentCtx, nullptr);
    EXPECT_EQ(parentCtx->depth(), 1u) << "parent scope must be on the stack";

    parentCtx->record(makeContentDep(pools, "/parent/before.nix", "pb"));

    // Push a child scope on top of the parent.
    {
        DepCaptureScope childScope(pools);
        auto * childCtx = childScope.ctx;
        ASSERT_NE(childCtx, nullptr);
        // Both scopes share the same DepRecordingContext when using the
        // standalone path — depth increments.
        EXPECT_EQ(childCtx->depth(), 2u) << "child scope must add a second level";

        childCtx->record(makeContentDep(pools, "/child/dep.nix", "cd"));

        // finalizeAndTakeDeps pops the child scope.
        auto childDeps = childScope.finalizeAndTakeDeps();

        EXPECT_EQ(keys(pools, childDeps), (std::vector<std::string>{"/child/dep.nix"}));

        // After the call, the child scope is gone — depth is back to 1.
        EXPECT_EQ(childCtx->depth(), 1u)
            << "finalizeAndTakeDeps must pop the child scope (Theme 11+12)";
        EXPECT_TRUE(childScope.scopePopped)
            << "scopePopped flag must be set after finalizeAndTakeDeps";

        // Record more deps — they go to the parent scope, not the popped child.
        parentCtx->record(makeContentDep(pools, "/parent/after.nix", "pa"));

        // Destructor runs here.  scopePopped == true → no second pop.
    }

    // Parent scope is still active (depth == 1).
    EXPECT_EQ(parentCtx->depth(), 1u)
        << "destructor must not pop twice when scopePopped is already set";

    // Parent collects its own deps only; child deps are gone.
    auto parentDeps = parentScope.finalizeAndTakeDeps();
    EXPECT_EQ(
        keys(pools, parentDeps),
        (std::vector<std::string>{"/parent/before.nix", "/parent/after.nix"}))
        << "parent must hold only its own deps; child dep must not leak";
}

// ═════════════════════════════════════════════════════════════════════
// Test 3: FinalizeAndTakeDeps_DestructorDoesNotDoublePop (Theme 11+12)
//
// If finalizeAndTakeDeps() is called and then the scope object goes out
// of scope, the destructor must NOT call popScope again (it checks
// scopePopped).  A double-pop would assert-fail or corrupt the stack.
// This test verifies no crash occurs.
// ═════════════════════════════════════════════════════════════════════

TEST_F(EpochBugTest, Epoch_FinalizeAndTakeDeps_DestructorDoesNotDoublePop)
{
    // Wrap in a block so we can observe post-destruction state.
    {
        DepCaptureScope scope(pools);
        auto * ctx = scope.ctx;
        ASSERT_NE(ctx, nullptr);

        ctx->record(makeContentDep(pools, "/some/dep.nix", "sd"));

        // Explicitly take deps and pop.
        auto deps = scope.finalizeAndTakeDeps();
        EXPECT_EQ(deps.size(), 1u);
        EXPECT_TRUE(scope.scopePopped);

        // Verify the stack is empty before destructor runs.
        EXPECT_EQ(ctx->depth(), 0u);

        // Destructor runs at end of block.  If it tries to pop an empty
        // stack (double-pop), it would assert.  No crash = test passes.
    }
    // If we reach here the destructor did not double-pop.
}

// ═════════════════════════════════════════════════════════════════════
// Test 4: DestructorWithoutFinalize_PopsScope
//
// If finalizeAndTakeDeps() is never called (e.g. exception path), the
// destructor must still pop the scope so the parent context is clean.
// ═════════════════════════════════════════════════════════════════════

TEST_F(EpochBugTest, Epoch_DestructorWithoutFinalize_PopsScope)
{
    // Create a parent scope so we can verify the depth returns to 1.
    DepCaptureScope outerScope(pools);
    auto * ctx = outerScope.ctx;
    ASSERT_NE(ctx, nullptr);
    EXPECT_EQ(ctx->depth(), 1u);

    ctx->record(makeContentDep(pools, "/outer/dep.nix", "od"));

    {
        // Inner scope: finalizeAndTakeDeps is NOT called.
        DepCaptureScope innerScope(pools);
        EXPECT_EQ(ctx->depth(), 2u) << "inner scope must push onto the stack";
        ctx->record(makeContentDep(pools, "/inner/dep.nix", "id"));

        // Let innerScope go out of scope WITHOUT calling finalizeAndTakeDeps.
        // Destructor path: scopePopped == false → destructor pops the scope.
    }

    // Depth must be back to 1 after destructor ran.
    EXPECT_EQ(ctx->depth(), 1u)
        << "destructor must pop the scope when finalizeAndTakeDeps was not called";

    // Outer scope can still collect its own deps normally.
    auto outerDeps = outerScope.finalizeAndTakeDeps();
    EXPECT_EQ(
        keys(pools, outerDeps),
        (std::vector<std::string>{"/outer/dep.nix"}))
        << "outer scope must contain only its own dep; inner dep must not leak";
}

// ═════════════════════════════════════════════════════════════════════
// Test 5: MergeIntoParent_PreservesEpochMapEntries (BUG-3)
//
// Integration test using TraceCacheFixture (provides EvalState +
// TraceSession). Exercises the coercion path that constructs a
// PublicationWarmupScope inside EvalState::coerceToContextObjectForUnsafeDiscard:
//
//   builtins.unsafeDiscardStringContext (builtins.fromJSON (builtins.readFile PATH))
//
// Evaluation flow (cold eval):
//   1. unsafeDiscardStringContext calls coerceToContextObjectForUnsafeDiscard,
//      which creates a PublicationWarmupScope and pushes a fresh dep scope.
//   2. Inside the scope, forceValue forces the fromJSON(readFile PATH) thunk,
//      recording the Content dep for PATH in the epoch log and epochMap.
//   3. coerceToContextObject returns PreservedString (isDetached() == false),
//      so mergeIntoParent() is called instead of discard().
//   4. With the fix: mergeIntoParent does NOT call rollbackReplayEpoch, so
//      the epochMap entry for the readFile thunk survives and the file dep
//      is preserved in the parent scope's dep recording.
//   5. Without the fix (BUG-3): rollbackReplayEpoch erased the epochMap
//      entry, the file dep was silently dropped, and after the file changed
//      the stale trace was wrongly served as a cache hit.
//
// The test verifies end-to-end:
//   - Cold eval records the file dep in the trace.
//   - Warm eval serves from trace (loaderCalls == 0, file unchanged).
//   - After file modification, the trace is invalidated (loaderCalls == 1).
// ═════════════════════════════════════════════════════════════════════

class EpochBugIntegrationTest : public TraceCacheFixture
{
public:
    EpochBugIntegrationTest()
    {
        testFingerprint = hashString(HashAlgorithm::SHA256, "epoch-bug-integration");
    }
};

TEST_F(EpochBugIntegrationTest, Epoch_MergeIntoParent_PreservesEpochMapEntries)
{
    // JSON file contains a bare string value so fromJSON returns an nString,
    // and unsafeDiscardStringContext triggers PublicationWarmupScope via
    // coerceToContextObjectForUnsafeDiscard → PreservedString → mergeIntoParent.
    TempJsonFile jsonFile(R"("hello")");

    auto expr = fmt(
        R"(builtins.unsafeDiscardStringContext (builtins.fromJSON (builtins.readFile "%s")))",
        jsonFile.path.string());

    // Eval 1 (cold): forces readFile sub-thunk inside PublicationWarmupScope.
    // mergeIntoParent must NOT call rollbackReplayEpoch; the file Content dep
    // must be preserved so it is written into the recorded trace.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Eval 2 (warm): the trace written in Eval 1's background writer should
    // include the file dep.  The file is unchanged → verification succeeds →
    // loaderCalls == 0 (served from cache).
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0)
            << "warm hit: file dep is valid, trace should be served from cache";
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Modify the JSON file — the Content dep for the readFile sub-thunk
    // must now hash-mismatch the stored dep.
    jsonFile.modify(R"("world!!")");
    invalidateFileCache(jsonFile.path);

    // Eval 3 (stale): the recorded trace must be invalidated because the file
    // dep was preserved through mergeIntoParent (BUG-3 fix).
    // Regression: if mergeIntoParent called rollbackReplayEpoch, the epochMap
    // entry for the readFile thunk was erased, the file dep was dropped from
    // the trace, and the stale trace was served as a cache hit (loaderCalls == 0).
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1)
            << "BUG-3: file dep must survive mergeIntoParent and invalidate "
               "the trace when the file changes; loaderCalls == 0 means the "
               "dep was silently dropped (rollbackReplayEpoch regression)";
        EXPECT_THAT(v, IsStringEq("world!!"));
    }
}

} // namespace nix::eval_trace
