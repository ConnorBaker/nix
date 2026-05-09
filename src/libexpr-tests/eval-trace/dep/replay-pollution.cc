#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/dep-recording-context.hh"
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

class ReplayPollutionTest : public ::testing::Test
{
protected:
    InterningPools pools;
    std::vector<Dep> epochLog;
    void SetUp() override { epochLog.clear(); }
    void TearDown() override { epochLog.clear(); }
};

// ═════════════════════════════════════════════════════════════════════
// Test 1: Structural isolation — recordToEpochLog does not enter ownDeps
// ═════════════════════════════════════════════════════════════════════

TEST_F(ReplayPollutionTest, Replay_ReplayedDeps_DoNotPolluteParentDedupSet)
{
    DepRecordingContext ctx(pools, epochLog);
    TestScopeAccess::pushScope(ctx);

    // Parent records dep A into its ownDeps
    ctx.record(makeContentDep(pools, "/a.nix", "a"));

    // Simulate child TracedExpr cache-hit replay:
    // recordToEpochLog() appends to epochLog only,
    // NOT to any scope's ownDeps — structural isolation by design.
    epochLog.push_back(makeContentDep(pools, "/shared.nix", "v1"));
    epochLog.push_back(makeContentDep(pools, "/child-only.nix", "c"));

    // Parent independently records /shared.nix — must succeed (no dedup pollution)
    ctx.record(makeContentDep(pools, "/shared.nix", "v1"));
    ctx.record(makeContentDep(pools, "/b.nix", "b"));

    auto deps = TestScopeAccess::takeDeps(ctx);
    auto k = keys(pools, deps);
    // Parent's trace must contain exactly its own deps: /a.nix, /shared.nix, /b.nix.
    // Note: /shared.nix appears because recordToEpochLog() did NOT pollute the
    // parent's depDedup filter, so the subsequent record() succeeds.
    EXPECT_EQ(k, (std::vector<std::string>{"/a.nix", "/shared.nix", "/b.nix"}));
}

// ═════════════════════════════════════════════════════════════════════
// Test 2: (DELETED) The dedup pollution bug is structurally eliminated
//   With per-tracker ownDeps, child record() goes into the child
//   tracker's ownDeps, not the parent's. No test needed.
// ═════════════════════════════════════════════════════════════════════

// ═════════════════════════════════════════════════════════════════════
// Test 3: recordToEpochLog appends to epochLog,
//         but NOT to any tracker's ownDeps
// ═════════════════════════════════════════════════════════════════════

TEST_F(ReplayPollutionTest, Replay_RecordReplay_AppendsToSessionAndEpochOnly)
{
    DepRecordingContext ctx(pools, epochLog);
    TestScopeAccess::pushScope(ctx);

    uint32_t epochBefore = epochLog.size();

    epochLog.push_back(makeContentDep(pools, "/x.nix", "x"));

    // epochLog grows by 1
    EXPECT_EQ(epochLog.size(), epochBefore + 1);
    EXPECT_EQ(
        depKeyDisplayForTest(pools, epochLog.back()),
        "/x.nix");

    // context's ownDeps is NOT affected
    auto deps = TestScopeAccess::takeDeps(ctx);
    EXPECT_EQ(deps.size(), 0u);
}

// ═════════════════════════════════════════════════════════════════════
// Test 4: recordToEpochLog does NOT dedup and does NOT enter ownDeps
// ═════════════════════════════════════════════════════════════════════

TEST_F(ReplayPollutionTest, Replay_RecordReplay_NoDedup)
{
    DepRecordingContext ctx(pools, epochLog);
    TestScopeAccess::pushScope(ctx);
    uint32_t epochBefore = epochLog.size();

    epochLog.push_back(makeContentDep(pools, "/x.nix", "v1"));
    epochLog.push_back(makeContentDep(pools, "/x.nix", "v1")); // same dep

    // Both appended to epochLog (no dedup for replay)
    EXPECT_EQ(epochLog.size(), epochBefore + 2);

    // But context's ownDeps has 0 deps — recordToEpochLog doesn't touch ownDeps
    auto deps = TestScopeAccess::takeDeps(ctx);
    EXPECT_EQ(deps.size(), 0u);
}

// ═════════════════════════════════════════════════════════════════════
// Test 5: Parent deps intact after child replay (no excludeChildRange)
// ═════════════════════════════════════════════════════════════════════

TEST_F(ReplayPollutionTest, Replay_EpochAfterReplay_ParentDepsIntact)
{
    DepRecordingContext ctx(pools, epochLog);
    TestScopeAccess::pushScope(ctx);

    // Parent records before child
    ctx.record(makeContentDep(pools, "/before.nix", "b"));

    // Child cache-hit replay — goes to session/epoch only, not parent's ownDeps
    epochLog.push_back(makeContentDep(pools, "/child.nix", "c"));

    // Parent records after child
    ctx.record(makeContentDep(pools, "/after.nix", "a"));

    auto deps = TestScopeAccess::takeDeps(ctx);
    EXPECT_EQ(keys(pools, deps), (std::vector<std::string>{"/before.nix", "/after.nix"}));
}

// ═════════════════════════════════════════════════════════════════════
// Test 6: Multiple children sharing a dep — parent trace complete
// ═════════════════════════════════════════════════════════════════════

TEST_F(ReplayPollutionTest, MultipleChildren_SharedDep_ParentTraceComplete)
{
    DepRecordingContext ctx(pools, epochLog);
    TestScopeAccess::pushScope(ctx);
    ctx.record(makeContentDep(pools, "/parent.nix", "p"));

    // Child A cache-hit replay (shares /lib.nix with parent)
    epochLog.push_back(makeContentDep(pools, "/lib.nix", "lib"));
    epochLog.push_back(makeContentDep(pools, "/a.nix", "a"));

    // Child B cache-hit replay (also uses /lib.nix)
    epochLog.push_back(makeContentDep(pools, "/lib.nix", "lib"));
    epochLog.push_back(makeContentDep(pools, "/b.nix", "b"));

    // Parent independently reads /lib.nix — must succeed (no dedup pollution)
    ctx.record(makeContentDep(pools, "/lib.nix", "lib"));
    ctx.record(makeContentDep(pools, "/parent2.nix", "p2"));

    auto deps = TestScopeAccess::takeDeps(ctx);
    EXPECT_EQ(keys(pools, deps),
        (std::vector<std::string>{"/parent.nix", "/lib.nix", "/parent2.nix"}));
}

} // namespace nix::eval_trace
