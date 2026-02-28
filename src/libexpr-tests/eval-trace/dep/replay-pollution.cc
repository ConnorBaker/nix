#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/dependency-tracker.hh"
#include "nix/expr/eval-trace-deps.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

/// Extract dep keys from a dep vector for exact-match assertions.
static std::vector<std::string> keys(const std::vector<CompactDep> & deps)
{
    std::vector<std::string> out;
    out.reserve(deps.size());
    for (auto & d : deps)
        out.push_back(std::string(resolveDepKey(d.keyId)));
    return out;
}

class ReplayPollutionTest : public ::testing::Test
{
protected:
    void SetUp() override { DependencyTracker::clearSessionTraces(); }
    void TearDown() override { DependencyTracker::clearSessionTraces(); }
};

// ═════════════════════════════════════════════════════════════════════
// Test 1: recordReplay does NOT pollute parent dedup set
// ═════════════════════════════════════════════════════════════════════

TEST_F(ReplayPollutionTest, ReplayedDeps_DoNotPolluteParentDedupSet)
{
    DependencyTracker parent;

    // Parent records dep A
    DependencyTracker::record(makeContentDep("/a.nix", "a"));

    // Simulate child TracedExpr cache-hit replay:
    // record child's deps (including /shared.nix) into excluded range
    uint32_t childStart = DependencyTracker::sessionTraces.size();
    DependencyTracker::recordReplay(makeContentDep("/shared.nix", "v1"));
    DependencyTracker::recordReplay(makeContentDep("/child-only.nix", "c"));
    uint32_t childEnd = DependencyTracker::sessionTraces.size();
    parent.excludeChildRange(childStart, childEnd);

    // Parent independently records /shared.nix — MUST NOT be deduped
    DependencyTracker::record(makeContentDep("/shared.nix", "v1"));
    DependencyTracker::record(makeContentDep("/b.nix", "b"));

    auto deps = parent.collectTraces();
    auto k = keys(deps);
    // Parent's trace must contain /a.nix, /shared.nix, /b.nix
    EXPECT_EQ(k, (std::vector<std::string>{"/a.nix", "/shared.nix", "/b.nix"}));
}

// ═════════════════════════════════════════════════════════════════════
// Test 2: Documents the bug — record() DOES pollute parent dedup set
// ═════════════════════════════════════════════════════════════════════

TEST_F(ReplayPollutionTest, RecordPollutesParentDedupSet_Demonstration)
{
    DependencyTracker parent;
    DependencyTracker::record(makeContentDep("/a.nix", "a"));

    // Simulate child replay using record() (the old buggy path)
    uint32_t childStart = DependencyTracker::sessionTraces.size();
    DependencyTracker::record(makeContentDep("/shared.nix", "v1"));
    uint32_t childEnd = DependencyTracker::sessionTraces.size();
    parent.excludeChildRange(childStart, childEnd);

    // Parent tries to record /shared.nix — dedup drops it!
    DependencyTracker::record(makeContentDep("/shared.nix", "v1"));

    auto deps = parent.collectTraces();
    // BUG: /shared.nix is missing because record() deduped against recordedKeys
    // which was polluted by the child's replay
    EXPECT_EQ(keys(deps), (std::vector<std::string>{"/a.nix"}));
    // This test documents the bug; it would need updating if we
    // change record() semantics (but we're adding recordReplay instead).
}

// ═════════════════════════════════════════════════════════════════════
// Test 3: recordReplay appends to sessionTraces (needed for epochs)
// ═════════════════════════════════════════════════════════════════════

TEST_F(ReplayPollutionTest, RecordReplay_AppendsToSessionTraces)
{
    DependencyTracker tracker;
    uint32_t before = DependencyTracker::sessionTraces.size();
    DependencyTracker::recordReplay(makeContentDep("/x.nix", "x"));
    uint32_t after = DependencyTracker::sessionTraces.size();
    EXPECT_EQ(after, before + 1);
    EXPECT_EQ(resolveDepKey(DependencyTracker::sessionTraces.back().keyId), "/x.nix");
}

// ═════════════════════════════════════════════════════════════════════
// Test 4: recordReplay does NOT dedup (no interaction with recordedKeys)
// ═════════════════════════════════════════════════════════════════════

TEST_F(ReplayPollutionTest, RecordReplay_NoDedup)
{
    DependencyTracker tracker;
    DependencyTracker::recordReplay(makeContentDep("/x.nix", "v1"));
    DependencyTracker::recordReplay(makeContentDep("/x.nix", "v1")); // same dep
    // Both appended (no dedup for replay)
    auto deps = tracker.collectTraces();
    EXPECT_EQ(deps.size(), 2u);
}

// ═════════════════════════════════════════════════════════════════════
// Test 5: Parent deps intact after child replay with excluded range
// ═════════════════════════════════════════════════════════════════════

TEST_F(ReplayPollutionTest, EpochAfterReplay_ParentDepsIntact)
{
    DependencyTracker parent;

    // Parent records before child
    DependencyTracker::record(makeContentDep("/before.nix", "b"));

    // Child cache-hit replay (excluded range)
    uint32_t cs = DependencyTracker::sessionTraces.size();
    DependencyTracker::recordReplay(makeContentDep("/child.nix", "c"));
    uint32_t ce = DependencyTracker::sessionTraces.size();
    parent.excludeChildRange(cs, ce);

    // Parent records after child
    DependencyTracker::record(makeContentDep("/after.nix", "a"));

    auto deps = parent.collectTraces();
    EXPECT_EQ(keys(deps), (std::vector<std::string>{"/before.nix", "/after.nix"}));
}

// ═════════════════════════════════════════════════════════════════════
// Test 6: Multiple children sharing a dep — parent trace complete
// ═════════════════════════════════════════════════════════════════════

TEST_F(ReplayPollutionTest, MultipleChildren_SharedDep_ParentTraceComplete)
{
    DependencyTracker parent;
    DependencyTracker::record(makeContentDep("/parent.nix", "p"));

    // Child A cache-hit replay (shares /lib.nix with parent)
    uint32_t c1s = DependencyTracker::sessionTraces.size();
    DependencyTracker::recordReplay(makeContentDep("/lib.nix", "lib"));
    DependencyTracker::recordReplay(makeContentDep("/a.nix", "a"));
    uint32_t c1e = DependencyTracker::sessionTraces.size();
    parent.excludeChildRange(c1s, c1e);

    // Child B cache-hit replay (also uses /lib.nix)
    uint32_t c2s = DependencyTracker::sessionTraces.size();
    DependencyTracker::recordReplay(makeContentDep("/lib.nix", "lib"));
    DependencyTracker::recordReplay(makeContentDep("/b.nix", "b"));
    uint32_t c2e = DependencyTracker::sessionTraces.size();
    parent.excludeChildRange(c2s, c2e);

    // Parent independently reads /lib.nix — must succeed
    DependencyTracker::record(makeContentDep("/lib.nix", "lib"));
    DependencyTracker::record(makeContentDep("/parent2.nix", "p2"));

    auto deps = parent.collectTraces();
    EXPECT_EQ(keys(deps),
        (std::vector<std::string>{"/parent.nix", "/lib.nix", "/parent2.nix"}));
}

} // namespace nix::eval_trace
