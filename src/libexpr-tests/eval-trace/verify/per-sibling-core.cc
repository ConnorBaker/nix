#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/deps/hash.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

/**
 * Tests for per-sibling ParentContext deps: verifying that adding/removing
 * unused siblings doesn't cascade to traces that don't depend on them.
 *
 * These tests construct traces with per-sibling deps manually (simulating
 * what SiblingAccessTracker produces) and verify behavior through the
 * TraceStore API.
 *
 * This file covers: basic tests, trace survival, multi-level cascading,
 * multiple deps, and recovery tests.
 */
class PerSiblingInvalidationTest : public TraceStoreFixture
{
protected:
    // Use a distinct context hash from the base class
    static constexpr int64_t testContextHash = 0xABCDEF0123456789;

    TraceStore makeDb()
    {
        return TraceStore(state.symbols, *state.traceCtx->pools,
            state.traceCtx->getVocabStore(state.symbols), testContextHash);
    }
};

// -- Per-sibling ParentContext deps: trace survival -------------------

TEST_F(PerSiblingInvalidationTest, TraceSurvivesUnrelatedSiblingAdd)
{
    // Child has per-sibling dep on sibA. Adding sibB doesn't invalidate child.
    ScopedEnvVar envA("NIX_PS_A", "val-a");
    ScopedEnvVar envB("NIX_PS_B", "val-b");

    auto db = makeDb();

    auto sibAPathId = vpath({"parent", "sibA"});
    auto sibBPathId = vpath({"parent", "sibB"});
    auto childPathId = vpath({"parent", "child"});

    // Record sibling A trace
    db.record(sibAPathId, string_t{"sibA-val", {}},
              {makeEnvVarDep(pools(), "NIX_PS_A", "val-a")});
    auto sibAHash = db.getCurrentTraceHash(sibAPathId);
    ASSERT_TRUE(sibAHash.has_value());

    // Record child with per-sibling dep on sibA only
    db.record(childPathId, string_t{"child-val", {}},
              {makeParentContextDep(vpath({"parent", "sibA"}), *sibAHash)});

    // Add new sibling B (no existing deps reference it)
    db.record(sibBPathId, string_t{"sibB-val", {}},
              {makeEnvVarDep(pools(), "NIX_PS_B", "val-b")});

    db.clearSessionCaches();

    // Child's per-sibling dep on sibA still passes -> verify succeeds
    auto result = db.verify(childPathId, {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-val", {}}, result->value, state.symbols);
}

TEST_F(PerSiblingInvalidationTest, TraceInvalidatedWhenAccessedSiblingChanges)
{
    // Child has per-sibling dep on sib. Changing sib invalidates child.
    ScopedEnvVar env("NIX_PS_C", "val1");

    auto db = makeDb();

    auto sibPathId = vpath({"parent", "sib"});
    auto childPathId = vpath({"parent", "child"});

    // Record sibling with val1
    db.record(sibPathId, string_t{"sib-v1", {}},
              {makeEnvVarDep(pools(), "NIX_PS_C", "val1")});
    auto sibHash1 = db.getCurrentTraceHash(sibPathId);
    ASSERT_TRUE(sibHash1.has_value());

    // Record child with per-sibling dep on sib
    db.record(childPathId, string_t{"child-v1", {}},
              {makeParentContextDep(vpath({"parent", "sib"}), *sibHash1)});

    // Change sibling's dep -> new trace hash
    setenv("NIX_PS_C", "val2", 1);
    db.record(sibPathId, string_t{"sib-v2", {}},
              {makeEnvVarDep(pools(), "NIX_PS_C", "val2")});

    db.clearSessionCaches();

    // Child's per-sibling dep on sib fails -> verify fails
    auto result = db.verify(childPathId, {}, state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(PerSiblingInvalidationTest, ZeroDepsZeroSiblings_WholeParentFallback)
{
    // Child with zero deps and zero sibling accesses uses whole-parent dep.
    // Changing parent invalidates child.
    ScopedEnvVar env("NIX_PS_P", "parent-val");

    auto db = makeDb();

    auto parentPathId = vpath({"parent"});
    auto childPathId = vpath({"parent", "child"});

    // Record parent
    db.record(parentPathId, string_t{"parent-result", {}},
              {makeEnvVarDep(pools(), "NIX_PS_P", "parent-val")});
    auto parentHash = db.getCurrentTraceHash(parentPathId);
    ASSERT_TRUE(parentHash.has_value());

    // Record child with whole-parent dep (zero sibling accesses fallback)
    db.record(childPathId, string_t{"child-val", {}},
              {makeParentContextDep(vpath({"parent"}), *parentHash)});

    // Change parent -> new trace hash
    setenv("NIX_PS_P", "parent-val-new", 1);
    db.record(parentPathId, string_t{"parent-result-new", {}},
              {makeEnvVarDep(pools(), "NIX_PS_P", "parent-val-new")});

    db.clearSessionCaches();

    // Child's whole-parent dep fails -> verify fails
    auto result = db.verify(childPathId, {}, state);
    EXPECT_FALSE(result.has_value());
}

// -- Multi-level cascading --------------------------------------------

TEST_F(PerSiblingInvalidationTest, MultiLevelSurvival_GrandchildUnaffected)
{
    // Three levels: root -> mid -> leaf. Each has per-sibling deps on its level.
    // Adding an unused root-level attr doesn't cascade to mid or leaf.
    ScopedEnvVar envRoot("NIX_PS_ROOT", "root-val");
    ScopedEnvVar envMid("NIX_PS_MID", "mid-val");

    auto db = makeDb();

    auto midSibPathId = vpath({"root", "midSib"});
    auto midPathId = vpath({"root", "mid"});
    auto leafSibPathId = vpath({"root", "mid", "leafSib"});
    auto leafPathId = vpath({"root", "mid", "leaf"});

    // Record root-level sibling that mid accesses
    db.record(midSibPathId, string_t{"midSib-val", {}},
              {makeEnvVarDep(pools(), "NIX_PS_ROOT", "root-val")});
    auto midSibHash = db.getCurrentTraceHash(midSibPathId);
    ASSERT_TRUE(midSibHash.has_value());

    // Record mid with per-sibling dep on midSib + own dep
    db.record(midPathId, string_t{"mid-val", {}},
              {makeParentContextDep(vpath({"root", "midSib"}), *midSibHash),
               makeEnvVarDep(pools(), "NIX_PS_MID", "mid-val")});
    auto midHash = db.getCurrentTraceHash(midPathId);
    ASSERT_TRUE(midHash.has_value());

    // Record leaf sibling (under mid)
    db.record(leafSibPathId, string_t{"leafSib-val", {}},
              {makeEnvVarDep(pools(), "NIX_PS_MID", "mid-val")});
    auto leafSibHash = db.getCurrentTraceHash(leafSibPathId);
    ASSERT_TRUE(leafSibHash.has_value());

    // Record leaf with per-sibling dep on leafSib
    db.record(leafPathId, string_t{"leaf-val", {}},
              {makeParentContextDep(vpath({"root", "mid", "leafSib"}), *leafSibHash)});

    // Add unused root-level attribute (simulates adding a by-name package)
    auto newPkgPathId = vpath({"root", "newPkg"});
    db.record(newPkgPathId, string_t{"new-val", {}}, {});

    db.clearSessionCaches();

    // mid's per-sibling dep on midSib passes (midSib unchanged)
    auto midResult = db.verify(midPathId, {}, state);
    ASSERT_TRUE(midResult.has_value());

    // leaf's per-sibling dep on leafSib passes (leafSib unchanged because mid unchanged)
    auto leafResult = db.verify(leafPathId, {}, state);
    ASSERT_TRUE(leafResult.has_value());
    assertCachedResultEquals(string_t{"leaf-val", {}}, leafResult->value, state.symbols);
}

TEST_F(PerSiblingInvalidationTest, MultiLevel_ChangeAtRoot_CascadesToDependents)
{
    // Changing a root-level sibling that mid accesses should cascade to mid
    // and then to leaf (since leaf depends on leafSib which depends on mid's env).
    ScopedEnvVar envRoot("NIX_PS_MLC_ROOT", "val1");
    ScopedEnvVar envMid("NIX_PS_MLC_MID", "mid-val");

    auto db = makeDb();

    auto midSibPathId = vpath({"root", "midSib"});
    auto midPathId = vpath({"root", "mid"});

    // Record root-level sibling
    db.record(midSibPathId, string_t{"midSib-v1", {}},
              {makeEnvVarDep(pools(), "NIX_PS_MLC_ROOT", "val1")});
    auto midSibHash = db.getCurrentTraceHash(midSibPathId);
    ASSERT_TRUE(midSibHash.has_value());

    // Record mid with per-sibling dep on midSib
    db.record(midPathId, string_t{"mid-v1", {}},
              {makeParentContextDep(vpath({"root", "midSib"}), *midSibHash)});

    // Change root env -> midSib re-evaluates -> new trace hash
    setenv("NIX_PS_MLC_ROOT", "val2", 1);
    db.record(midSibPathId, string_t{"midSib-v2", {}},
              {makeEnvVarDep(pools(), "NIX_PS_MLC_ROOT", "val2")});

    db.clearSessionCaches();

    // mid's per-sibling dep on midSib fails (midSib's trace hash changed)
    auto midResult = db.verify(midPathId, {}, state);
    EXPECT_FALSE(midResult.has_value());
}

// -- Multiple per-sibling deps ----------------------------------------

TEST_F(PerSiblingInvalidationTest, MultiplePerSiblingDeps_AllPass)
{
    ScopedEnvVar envA("NIX_PS_MA", "val-a");
    ScopedEnvVar envB("NIX_PS_MB", "val-b");

    auto db = makeDb();

    auto sibAPathId = vpath({"parent", "sibA"});
    auto sibBPathId = vpath({"parent", "sibB"});
    auto childPathId = vpath({"parent", "child"});

    // Record two siblings
    db.record(sibAPathId, string_t{"a-val", {}},
              {makeEnvVarDep(pools(), "NIX_PS_MA", "val-a")});
    db.record(sibBPathId, string_t{"b-val", {}},
              {makeEnvVarDep(pools(), "NIX_PS_MB", "val-b")});
    auto sibAHash = db.getCurrentTraceHash(sibAPathId);
    auto sibBHash = db.getCurrentTraceHash(sibBPathId);
    ASSERT_TRUE(sibAHash.has_value());
    ASSERT_TRUE(sibBHash.has_value());

    // Record child with per-sibling deps on both siblings
    db.record(childPathId, string_t{"child-val", {}},
              {makeParentContextDep(vpath({"parent", "sibA"}), *sibAHash),
               makeParentContextDep(vpath({"parent", "sibB"}), *sibBHash)});

    db.clearSessionCaches();

    // Both sibling deps pass -> verify succeeds
    auto result = db.verify(childPathId, {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-val", {}}, result->value, state.symbols);
}

TEST_F(PerSiblingInvalidationTest, MultiplePerSiblingDeps_OneFails)
{
    ScopedEnvVar envA("NIX_PS_MFA", "val-a");
    ScopedEnvVar envB("NIX_PS_MFB", "val-b1");

    auto db = makeDb();

    auto sibAPathId = vpath({"parent", "sibA"});
    auto sibBPathId = vpath({"parent", "sibB"});
    auto childPathId = vpath({"parent", "child"});

    // Record two siblings
    db.record(sibAPathId, string_t{"a-val", {}},
              {makeEnvVarDep(pools(), "NIX_PS_MFA", "val-a")});
    db.record(sibBPathId, string_t{"b-val", {}},
              {makeEnvVarDep(pools(), "NIX_PS_MFB", "val-b1")});
    auto sibAHash = db.getCurrentTraceHash(sibAPathId);
    auto sibBHash = db.getCurrentTraceHash(sibBPathId);
    ASSERT_TRUE(sibAHash.has_value());
    ASSERT_TRUE(sibBHash.has_value());

    // Record child with per-sibling deps on both
    db.record(childPathId, string_t{"child-val", {}},
              {makeParentContextDep(vpath({"parent", "sibA"}), *sibAHash),
               makeParentContextDep(vpath({"parent", "sibB"}), *sibBHash)});

    // Change sibB -> new trace hash
    setenv("NIX_PS_MFB", "val-b2", 1);
    db.record(sibBPathId, string_t{"b-val-new", {}},
              {makeEnvVarDep(pools(), "NIX_PS_MFB", "val-b2")});

    db.clearSessionCaches();

    // sibB's trace hash changed -> child's per-sibling dep on sibB fails
    auto result = db.verify(childPathId, {}, state);
    EXPECT_FALSE(result.has_value());
}

// -- Recovery with per-sibling deps -----------------------------------

TEST_F(PerSiblingInvalidationTest, PerSiblingDep_RecoveryOnRevert)
{
    // Record child v1 with per-sibling dep, change sib to v2, record child v2,
    // revert sib to v1 -> recovery finds child v1.
    ScopedEnvVar env("NIX_PS_RV", "val1");

    auto db = makeDb();

    auto sibPathId = vpath({"parent", "sib"});
    auto childPathId = vpath({"parent", "child"});

    // Version 1: sib with val1
    db.record(sibPathId, string_t{"sib-v1", {}},
              {makeEnvVarDep(pools(), "NIX_PS_RV", "val1")});
    auto sibHash1 = db.getCurrentTraceHash(sibPathId);
    ASSERT_TRUE(sibHash1.has_value());

    // Child v1 with per-sibling dep
    db.record(childPathId, string_t{"child-v1", {}},
              {makeParentContextDep(sibPathId, *sibHash1)});

    // Version 2: sib with val2
    setenv("NIX_PS_RV", "val2", 1);
    db.record(sibPathId, string_t{"sib-v2", {}},
              {makeEnvVarDep(pools(), "NIX_PS_RV", "val2")});
    auto sibHash2 = db.getCurrentTraceHash(sibPathId);
    ASSERT_TRUE(sibHash2.has_value());

    // Child v2 with per-sibling dep
    db.record(childPathId, string_t{"child-v2", {}},
              {makeParentContextDep(sibPathId, *sibHash2)});

    // Revert sib to v1
    setenv("NIX_PS_RV", "val1", 1);
    db.record(sibPathId, string_t{"sib-v1", {}},
              {makeEnvVarDep(pools(), "NIX_PS_RV", "val1")});

    db.clearSessionCaches();

    // Recovery: child v1's per-sibling dep matches sib v1's trace hash
    auto result = db.verify(childPathId, {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-v1", {}}, result->value, state.symbols);
}

TEST_F(PerSiblingInvalidationTest, PerSiblingDepWithOwnDeps_BothMustPass)
{
    // Child has both own deps (EnvVar) and a per-sibling dep.
    // Verify passes only when both pass.
    ScopedEnvVar envOwn("NIX_PS_OWN", "own-val");
    ScopedEnvVar envSib("NIX_PS_SIBDEP", "sib-val");

    auto db = makeDb();

    auto sibPathId = vpath({"parent", "sib"});
    auto childPathId = vpath({"parent", "child"});

    // Record sibling
    db.record(sibPathId, string_t{"sib-result", {}},
              {makeEnvVarDep(pools(), "NIX_PS_SIBDEP", "sib-val")});
    auto sibHash = db.getCurrentTraceHash(sibPathId);
    ASSERT_TRUE(sibHash.has_value());

    // Record child with own dep + per-sibling dep
    db.record(childPathId, string_t{"child-result", {}},
              {makeEnvVarDep(pools(), "NIX_PS_OWN", "own-val"),
               makeParentContextDep(vpath({"parent", "sib"}), *sibHash)});

    db.clearSessionCaches();

    // Both pass -> verify succeeds
    auto result1 = db.verify(childPathId, {}, state);
    ASSERT_TRUE(result1.has_value());

    // Change only the child's own dep -> verify fails
    setenv("NIX_PS_OWN", "own-val-changed", 1);
    db.clearSessionCaches();
    auto result2 = db.verify(childPathId, {}, state);
    EXPECT_FALSE(result2.has_value());
}

} // namespace nix::eval_trace
