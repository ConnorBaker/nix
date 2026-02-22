#include "eval-trace/helpers.hh"
#include "nix/expr/trace-hash.hh"

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
        return TraceStore(state.symbols, testContextHash);
    }
};

// -- Per-sibling ParentContext deps: trace survival -------------------

TEST_F(PerSiblingInvalidationTest, TraceSurvivesUnrelatedSiblingAdd)
{
    // Child has per-sibling dep on sibA. Adding sibB doesn't invalidate child.
    ScopedEnvVar envA("NIX_PS_A", "val-a");
    ScopedEnvVar envB("NIX_PS_B", "val-b");

    auto db = makeDb();

    auto sibAPath = makePath({"parent", "sibA"});
    auto sibBPath = makePath({"parent", "sibB"});
    auto childPath = makePath({"parent", "child"});

    // Record sibling A trace
    db.record(sibAPath, string_t{"sibA-val", {}},
              {makeEnvVarDep("NIX_PS_A", "val-a")}, false);
    auto sibAHash = db.getCurrentTraceHash(sibAPath);
    ASSERT_TRUE(sibAHash.has_value());

    // Record child with per-sibling dep on sibA only
    db.record(childPath, string_t{"child-val", {}},
              {makeParentContextDep(toDepKey(sibAPath), *sibAHash)}, false);

    // Add new sibling B (no existing deps reference it)
    db.record(sibBPath, string_t{"sibB-val", {}},
              {makeEnvVarDep("NIX_PS_B", "val-b")}, false);

    db.clearSessionCaches();

    // Child's per-sibling dep on sibA still passes -> verify succeeds
    auto result = db.verify(childPath, {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-val", {}}, result->value, state.symbols);
}

TEST_F(PerSiblingInvalidationTest, TraceInvalidatedWhenAccessedSiblingChanges)
{
    // Child has per-sibling dep on sib. Changing sib invalidates child.
    ScopedEnvVar env("NIX_PS_C", "val1");

    auto db = makeDb();

    auto sibPath = makePath({"parent", "sib"});
    auto childPath = makePath({"parent", "child"});

    // Record sibling with val1
    db.record(sibPath, string_t{"sib-v1", {}},
              {makeEnvVarDep("NIX_PS_C", "val1")}, false);
    auto sibHash1 = db.getCurrentTraceHash(sibPath);
    ASSERT_TRUE(sibHash1.has_value());

    // Record child with per-sibling dep on sib
    db.record(childPath, string_t{"child-v1", {}},
              {makeParentContextDep(toDepKey(sibPath), *sibHash1)}, false);

    // Change sibling's dep -> new trace hash
    setenv("NIX_PS_C", "val2", 1);
    db.record(sibPath, string_t{"sib-v2", {}},
              {makeEnvVarDep("NIX_PS_C", "val2")}, false);

    db.clearSessionCaches();

    // Child's per-sibling dep on sib fails -> verify fails
    auto result = db.verify(childPath, {}, state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(PerSiblingInvalidationTest, ZeroDepsZeroSiblings_WholeParentFallback)
{
    // Child with zero deps and zero sibling accesses uses whole-parent dep.
    // Changing parent invalidates child.
    ScopedEnvVar env("NIX_PS_P", "parent-val");

    auto db = makeDb();

    auto parentPath = std::string("parent");
    auto childPath = makePath({"parent", "child"});

    // Record parent
    db.record(parentPath, string_t{"parent-result", {}},
              {makeEnvVarDep("NIX_PS_P", "parent-val")}, true);
    auto parentHash = db.getCurrentTraceHash(parentPath);
    ASSERT_TRUE(parentHash.has_value());

    // Record child with whole-parent dep (zero sibling accesses fallback)
    db.record(childPath, string_t{"child-val", {}},
              {makeParentContextDep(toDepKey(parentPath), *parentHash)}, false);

    // Change parent -> new trace hash
    setenv("NIX_PS_P", "parent-val-new", 1);
    db.record(parentPath, string_t{"parent-result-new", {}},
              {makeEnvVarDep("NIX_PS_P", "parent-val-new")}, true);

    db.clearSessionCaches();

    // Child's whole-parent dep fails -> verify fails
    auto result = db.verify(childPath, {}, state);
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

    auto midSibPath = makePath({"root", "midSib"});
    auto midPath = makePath({"root", "mid"});
    auto leafSibPath = makePath({"root", "mid", "leafSib"});
    auto leafPath = makePath({"root", "mid", "leaf"});

    // Record root-level sibling that mid accesses
    db.record(midSibPath, string_t{"midSib-val", {}},
              {makeEnvVarDep("NIX_PS_ROOT", "root-val")}, false);
    auto midSibHash = db.getCurrentTraceHash(midSibPath);
    ASSERT_TRUE(midSibHash.has_value());

    // Record mid with per-sibling dep on midSib + own dep
    db.record(midPath, string_t{"mid-val", {}},
              {makeParentContextDep(toDepKey(midSibPath), *midSibHash),
               makeEnvVarDep("NIX_PS_MID", "mid-val")}, false);
    auto midHash = db.getCurrentTraceHash(midPath);
    ASSERT_TRUE(midHash.has_value());

    // Record leaf sibling (under mid)
    db.record(leafSibPath, string_t{"leafSib-val", {}},
              {makeEnvVarDep("NIX_PS_MID", "mid-val")}, false);
    auto leafSibHash = db.getCurrentTraceHash(leafSibPath);
    ASSERT_TRUE(leafSibHash.has_value());

    // Record leaf with per-sibling dep on leafSib
    db.record(leafPath, string_t{"leaf-val", {}},
              {makeParentContextDep(toDepKey(leafSibPath), *leafSibHash)}, false);

    // Add unused root-level attribute (simulates adding a by-name package)
    auto newPkgPath = makePath({"root", "newPkg"});
    db.record(newPkgPath, string_t{"new-val", {}}, {}, false);

    db.clearSessionCaches();

    // mid's per-sibling dep on midSib passes (midSib unchanged)
    auto midResult = db.verify(midPath, {}, state);
    ASSERT_TRUE(midResult.has_value());

    // leaf's per-sibling dep on leafSib passes (leafSib unchanged because mid unchanged)
    auto leafResult = db.verify(leafPath, {}, state);
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

    auto midSibPath = makePath({"root", "midSib"});
    auto midPath = makePath({"root", "mid"});

    // Record root-level sibling
    db.record(midSibPath, string_t{"midSib-v1", {}},
              {makeEnvVarDep("NIX_PS_MLC_ROOT", "val1")}, false);
    auto midSibHash = db.getCurrentTraceHash(midSibPath);
    ASSERT_TRUE(midSibHash.has_value());

    // Record mid with per-sibling dep on midSib
    db.record(midPath, string_t{"mid-v1", {}},
              {makeParentContextDep(toDepKey(midSibPath), *midSibHash)}, false);

    // Change root env -> midSib re-evaluates -> new trace hash
    setenv("NIX_PS_MLC_ROOT", "val2", 1);
    db.record(midSibPath, string_t{"midSib-v2", {}},
              {makeEnvVarDep("NIX_PS_MLC_ROOT", "val2")}, false);

    db.clearSessionCaches();

    // mid's per-sibling dep on midSib fails (midSib's trace hash changed)
    auto midResult = db.verify(midPath, {}, state);
    EXPECT_FALSE(midResult.has_value());
}

// -- Multiple per-sibling deps ----------------------------------------

TEST_F(PerSiblingInvalidationTest, MultiplePerSiblingDeps_AllPass)
{
    ScopedEnvVar envA("NIX_PS_MA", "val-a");
    ScopedEnvVar envB("NIX_PS_MB", "val-b");

    auto db = makeDb();

    auto sibAPath = makePath({"parent", "sibA"});
    auto sibBPath = makePath({"parent", "sibB"});
    auto childPath = makePath({"parent", "child"});

    // Record two siblings
    db.record(sibAPath, string_t{"a-val", {}},
              {makeEnvVarDep("NIX_PS_MA", "val-a")}, false);
    db.record(sibBPath, string_t{"b-val", {}},
              {makeEnvVarDep("NIX_PS_MB", "val-b")}, false);
    auto sibAHash = db.getCurrentTraceHash(sibAPath);
    auto sibBHash = db.getCurrentTraceHash(sibBPath);
    ASSERT_TRUE(sibAHash.has_value());
    ASSERT_TRUE(sibBHash.has_value());

    // Record child with per-sibling deps on both siblings
    db.record(childPath, string_t{"child-val", {}},
              {makeParentContextDep(toDepKey(sibAPath), *sibAHash),
               makeParentContextDep(toDepKey(sibBPath), *sibBHash)}, false);

    db.clearSessionCaches();

    // Both sibling deps pass -> verify succeeds
    auto result = db.verify(childPath, {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-val", {}}, result->value, state.symbols);
}

TEST_F(PerSiblingInvalidationTest, MultiplePerSiblingDeps_OneFails)
{
    ScopedEnvVar envA("NIX_PS_MFA", "val-a");
    ScopedEnvVar envB("NIX_PS_MFB", "val-b1");

    auto db = makeDb();

    auto sibAPath = makePath({"parent", "sibA"});
    auto sibBPath = makePath({"parent", "sibB"});
    auto childPath = makePath({"parent", "child"});

    // Record two siblings
    db.record(sibAPath, string_t{"a-val", {}},
              {makeEnvVarDep("NIX_PS_MFA", "val-a")}, false);
    db.record(sibBPath, string_t{"b-val", {}},
              {makeEnvVarDep("NIX_PS_MFB", "val-b1")}, false);
    auto sibAHash = db.getCurrentTraceHash(sibAPath);
    auto sibBHash = db.getCurrentTraceHash(sibBPath);
    ASSERT_TRUE(sibAHash.has_value());
    ASSERT_TRUE(sibBHash.has_value());

    // Record child with per-sibling deps on both
    db.record(childPath, string_t{"child-val", {}},
              {makeParentContextDep(toDepKey(sibAPath), *sibAHash),
               makeParentContextDep(toDepKey(sibBPath), *sibBHash)}, false);

    // Change sibB -> new trace hash
    setenv("NIX_PS_MFB", "val-b2", 1);
    db.record(sibBPath, string_t{"b-val-new", {}},
              {makeEnvVarDep("NIX_PS_MFB", "val-b2")}, false);

    db.clearSessionCaches();

    // sibB's trace hash changed -> child's per-sibling dep on sibB fails
    auto result = db.verify(childPath, {}, state);
    EXPECT_FALSE(result.has_value());
}

// -- Recovery with per-sibling deps -----------------------------------

TEST_F(PerSiblingInvalidationTest, PerSiblingDep_RecoveryOnRevert)
{
    // Record child v1 with per-sibling dep, change sib to v2, record child v2,
    // revert sib to v1 -> recovery finds child v1.
    ScopedEnvVar env("NIX_PS_RV", "val1");

    auto db = makeDb();

    auto sibPath = makePath({"parent", "sib"});
    auto childPath = makePath({"parent", "child"});

    // Version 1: sib with val1
    db.record(sibPath, string_t{"sib-v1", {}},
              {makeEnvVarDep("NIX_PS_RV", "val1")}, false);
    auto sibHash1 = db.getCurrentTraceHash(sibPath);
    ASSERT_TRUE(sibHash1.has_value());

    // Child v1 with per-sibling dep
    db.record(childPath, string_t{"child-v1", {}},
              {makeParentContextDep(toDepKey(sibPath), *sibHash1)}, false);

    // Version 2: sib with val2
    setenv("NIX_PS_RV", "val2", 1);
    db.record(sibPath, string_t{"sib-v2", {}},
              {makeEnvVarDep("NIX_PS_RV", "val2")}, false);
    auto sibHash2 = db.getCurrentTraceHash(sibPath);
    ASSERT_TRUE(sibHash2.has_value());

    // Child v2 with per-sibling dep
    db.record(childPath, string_t{"child-v2", {}},
              {makeParentContextDep(toDepKey(sibPath), *sibHash2)}, false);

    // Revert sib to v1
    setenv("NIX_PS_RV", "val1", 1);
    db.record(sibPath, string_t{"sib-v1", {}},
              {makeEnvVarDep("NIX_PS_RV", "val1")}, false);

    db.clearSessionCaches();

    // Recovery: child v1's per-sibling dep matches sib v1's trace hash
    auto result = db.verify(childPath, {}, state);
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

    auto sibPath = makePath({"parent", "sib"});
    auto childPath = makePath({"parent", "child"});

    // Record sibling
    db.record(sibPath, string_t{"sib-result", {}},
              {makeEnvVarDep("NIX_PS_SIBDEP", "sib-val")}, false);
    auto sibHash = db.getCurrentTraceHash(sibPath);
    ASSERT_TRUE(sibHash.has_value());

    // Record child with own dep + per-sibling dep
    db.record(childPath, string_t{"child-result", {}},
              {makeEnvVarDep("NIX_PS_OWN", "own-val"),
               makeParentContextDep(toDepKey(sibPath), *sibHash)}, false);

    db.clearSessionCaches();

    // Both pass -> verify succeeds
    auto result1 = db.verify(childPath, {}, state);
    ASSERT_TRUE(result1.has_value());

    // Change only the child's own dep -> verify fails
    setenv("NIX_PS_OWN", "own-val-changed", 1);
    db.clearSessionCaches();
    auto result2 = db.verify(childPath, {}, state);
    EXPECT_FALSE(result2.has_value());
}

} // namespace nix::eval_trace
