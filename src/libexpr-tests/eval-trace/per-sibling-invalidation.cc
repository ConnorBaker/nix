#include "helpers.hh"
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

// ── Per-sibling ParentContext deps: trace survival ───────────────────

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

    // Child's per-sibling dep on sibA still passes → verify succeeds
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

    // Change sibling's dep → new trace hash
    setenv("NIX_PS_C", "val2", 1);
    db.record(sibPath, string_t{"sib-v2", {}},
              {makeEnvVarDep("NIX_PS_C", "val2")}, false);

    db.clearSessionCaches();

    // Child's per-sibling dep on sib fails → verify fails
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

    // Change parent → new trace hash
    setenv("NIX_PS_P", "parent-val-new", 1);
    db.record(parentPath, string_t{"parent-result-new", {}},
              {makeEnvVarDep("NIX_PS_P", "parent-val-new")}, true);

    db.clearSessionCaches();

    // Child's whole-parent dep fails → verify fails
    auto result = db.verify(childPath, {}, state);
    EXPECT_FALSE(result.has_value());
}

// ── Multi-level cascading ────────────────────────────────────────────

TEST_F(PerSiblingInvalidationTest, MultiLevelSurvival_GrandchildUnaffected)
{
    // Three levels: root → mid → leaf. Each has per-sibling deps on its level.
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

    // Change root env → midSib re-evaluates → new trace hash
    setenv("NIX_PS_MLC_ROOT", "val2", 1);
    db.record(midSibPath, string_t{"midSib-v2", {}},
              {makeEnvVarDep("NIX_PS_MLC_ROOT", "val2")}, false);

    db.clearSessionCaches();

    // mid's per-sibling dep on midSib fails (midSib's trace hash changed)
    auto midResult = db.verify(midPath, {}, state);
    EXPECT_FALSE(midResult.has_value());
}

// ── Multiple per-sibling deps ────────────────────────────────────────

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

    // Both sibling deps pass → verify succeeds
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

    // Change sibB → new trace hash
    setenv("NIX_PS_MFB", "val-b2", 1);
    db.record(sibBPath, string_t{"b-val-new", {}},
              {makeEnvVarDep("NIX_PS_MFB", "val-b2")}, false);

    db.clearSessionCaches();

    // sibB's trace hash changed → child's per-sibling dep on sibB fails
    auto result = db.verify(childPath, {}, state);
    EXPECT_FALSE(result.has_value());
}

// ── Recovery with per-sibling deps ───────────────────────────────────

TEST_F(PerSiblingInvalidationTest, PerSiblingDep_RecoveryOnRevert)
{
    // Record child v1 with per-sibling dep, change sib to v2, record child v2,
    // revert sib to v1 → recovery finds child v1.
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

    // Both pass → verify succeeds
    auto result1 = db.verify(childPath, {}, state);
    ASSERT_TRUE(result1.has_value());

    // Change only the child's own dep → verify fails
    setenv("NIX_PS_OWN", "own-val-changed", 1);
    db.clearSessionCaches();
    auto result2 = db.verify(childPath, {}, state);
    EXPECT_FALSE(result2.has_value());
}

// ── Sibling removal ──────────────────────────────────────────────────

TEST_F(PerSiblingInvalidationTest, SiblingTraceHashChanges_ChildInvalidated)
{
    // When a sibling's trace hash changes (e.g., sibling re-recorded with
    // different deps), child's per-sibling dep fails.
    auto db = makeDb();

    auto sibPath = makePath({"parent", "sib"});
    auto childPath = makePath({"parent", "child"});

    // Record sibling with no deps
    db.record(sibPath, string_t{"sib-val", {}}, {}, false);
    auto sibHash = db.getCurrentTraceHash(sibPath);
    ASSERT_TRUE(sibHash.has_value());

    // Record child with per-sibling dep
    db.record(childPath, string_t{"child-val", {}},
              {makeParentContextDep(toDepKey(sibPath), *sibHash)}, false);

    // "Modify" sibling by recording with different deps → new trace hash
    db.record(sibPath, string_t{"sib-val-modified", {}},
              {makeEnvVarDep("NIX_PS_GONE", "x")}, false);

    db.clearSessionCaches();

    // Sibling's trace hash changed → child fails
    auto result = db.verify(childPath, {}, state);
    EXPECT_FALSE(result.has_value());
}

// ── Tab/null-byte key conversion ─────────────────────────────────────

TEST_F(PerSiblingInvalidationTest, DepKeyTabConversion_DeepPath)
{
    // Per-sibling dep key for a deeply nested sibling uses tab-separated path.
    // Verify that verification correctly converts back to null-byte path for DB lookup.
    ScopedEnvVar env("NIX_PS_DEEP", "val");

    auto db = makeDb();

    auto deepSibPath = makePath({"root", "mid", "deep", "sib"});
    auto childPath = makePath({"root", "mid", "deep", "child"});

    // Record deeply nested sibling
    db.record(deepSibPath, string_t{"deep-sib-val", {}},
              {makeEnvVarDep("NIX_PS_DEEP", "val")}, false);
    auto sibHash = db.getCurrentTraceHash(deepSibPath);
    ASSERT_TRUE(sibHash.has_value());

    // Record child with per-sibling dep using tab-separated key
    auto depKey = toDepKey(deepSibPath);
    // Verify the key has tabs (not null bytes)
    EXPECT_NE(depKey.find('\t'), std::string::npos);

    db.record(childPath, string_t{"child-val", {}},
              {makeParentContextDep(depKey, *sibHash)}, false);

    db.clearSessionCaches();

    // Verification should correctly resolve the tab-separated dep key
    auto result = db.verify(childPath, {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-val", {}}, result->value, state.symbols);
}

// ── Edge cases: fragile code paths ───────────────────────────────

TEST_F(PerSiblingInvalidationTest, SiblingRemovedFromDB_VerifyFails)
{
    // When a sibling referenced by a per-sibling dep no longer exists in
    // CurrentTraces, getCurrentTraceHash returns nullopt → verify fails.
    // This exercises the "missing sibling" path in verifyTrace.
    auto db = makeDb();

    auto sibPath = makePath({"parent", "sib"});
    auto childPath = makePath({"parent", "child"});

    // Record sibling, get its trace hash
    db.record(sibPath, string_t{"sib-val", {}}, {}, false);
    auto sibHash = db.getCurrentTraceHash(sibPath);
    ASSERT_TRUE(sibHash.has_value());

    // Record child with per-sibling dep on sib
    db.record(childPath, string_t{"child-val", {}},
              {makeParentContextDep(toDepKey(sibPath), *sibHash)}, false);

    // Overwrite sib with a completely new trace (different result + deps)
    // This changes the trace hash, simulating the sibling being
    // re-evaluated with different content.
    db.record(sibPath, string_t{"totally-different", {}},
              {makeEnvVarDep("NIX_PS_GONE_2", "x")}, false);

    db.clearSessionCaches();

    // Child's per-sibling dep on old sib trace hash fails
    auto result = db.verify(childPath, {}, state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(PerSiblingInvalidationTest, RootLevelPerSiblingDep_SingleComponentPath)
{
    // Root-level attributes have single-component paths with no null-byte
    // separators. Verify that the tab/null conversion is a no-op and
    // per-sibling deps work correctly at the top level.
    ScopedEnvVar env("NIX_PS_ROOT_SIB", "val");

    auto db = makeDb();

    auto sibPath = std::string("lib");       // single component, no separators
    auto childPath = std::string("hello");   // single component, no separators

    // Record root-level sibling
    db.record(sibPath, string_t{"lib-val", {}},
              {makeEnvVarDep("NIX_PS_ROOT_SIB", "val")}, false);
    auto sibHash = db.getCurrentTraceHash(sibPath);
    ASSERT_TRUE(sibHash.has_value());

    // Record child with per-sibling dep (toDepKey is identity for no-separator paths)
    auto depKey = toDepKey(sibPath);
    EXPECT_EQ(depKey, sibPath); // no separators to convert
    db.record(childPath, string_t{"hello-val", {}},
              {makeParentContextDep(depKey, *sibHash)}, false);

    db.clearSessionCaches();

    // Verify passes — sibling unchanged
    auto result = db.verify(childPath, {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"hello-val", {}}, result->value, state.symbols);
}

TEST_F(PerSiblingInvalidationTest, MultipleChildrenShareAccessedSibling)
{
    // Two children of the same parent both depend on the same sibling.
    // Changing the sibling should invalidate BOTH children.
    ScopedEnvVar env("NIX_PS_SHARED", "val1");

    auto db = makeDb();

    auto sibPath = makePath({"parent", "shared"});
    auto child1Path = makePath({"parent", "child1"});
    auto child2Path = makePath({"parent", "child2"});

    // Record shared sibling
    db.record(sibPath, string_t{"shared-val", {}},
              {makeEnvVarDep("NIX_PS_SHARED", "val1")}, false);
    auto sibHash = db.getCurrentTraceHash(sibPath);
    ASSERT_TRUE(sibHash.has_value());

    // Record both children with per-sibling dep on same sibling
    db.record(child1Path, string_t{"child1-val", {}},
              {makeParentContextDep(toDepKey(sibPath), *sibHash)}, false);
    db.record(child2Path, string_t{"child2-val", {}},
              {makeParentContextDep(toDepKey(sibPath), *sibHash)}, false);

    db.clearSessionCaches();

    // Both verify pass when sibling unchanged
    EXPECT_TRUE(db.verify(child1Path, {}, state).has_value());
    EXPECT_TRUE(db.verify(child2Path, {}, state).has_value());

    // Change sibling
    setenv("NIX_PS_SHARED", "val2", 1);
    db.record(sibPath, string_t{"shared-new", {}},
              {makeEnvVarDep("NIX_PS_SHARED", "val2")}, false);

    db.clearSessionCaches();

    // Both children should now fail
    EXPECT_FALSE(db.verify(child1Path, {}, state).has_value());
    EXPECT_FALSE(db.verify(child2Path, {}, state).has_value());
}

TEST_F(PerSiblingInvalidationTest, ChildWithDifferentSiblingSubsets)
{
    // Two children of the same parent access different subsets of siblings.
    // Changing a sibling only invalidates the child that depends on it.
    ScopedEnvVar envA("NIX_PS_CSA", "val-a");
    ScopedEnvVar envB("NIX_PS_CSB", "val-b");

    auto db = makeDb();

    auto sibAPath = makePath({"parent", "sibA"});
    auto sibBPath = makePath({"parent", "sibB"});
    auto child1Path = makePath({"parent", "child1"});
    auto child2Path = makePath({"parent", "child2"});

    // Record two siblings
    db.record(sibAPath, string_t{"a-val", {}},
              {makeEnvVarDep("NIX_PS_CSA", "val-a")}, false);
    db.record(sibBPath, string_t{"b-val", {}},
              {makeEnvVarDep("NIX_PS_CSB", "val-b")}, false);
    auto sibAHash = db.getCurrentTraceHash(sibAPath);
    auto sibBHash = db.getCurrentTraceHash(sibBPath);
    ASSERT_TRUE(sibAHash.has_value());
    ASSERT_TRUE(sibBHash.has_value());

    // child1 depends on sibA only, child2 depends on sibB only
    db.record(child1Path, string_t{"c1-val", {}},
              {makeParentContextDep(toDepKey(sibAPath), *sibAHash)}, false);
    db.record(child2Path, string_t{"c2-val", {}},
              {makeParentContextDep(toDepKey(sibBPath), *sibBHash)}, false);

    // Change sibA only
    setenv("NIX_PS_CSA", "val-a-new", 1);
    db.record(sibAPath, string_t{"a-val-new", {}},
              {makeEnvVarDep("NIX_PS_CSA", "val-a-new")}, false);

    db.clearSessionCaches();

    // child1 (depends on sibA) should fail
    EXPECT_FALSE(db.verify(child1Path, {}, state).has_value());
    // child2 (depends on sibB) should pass — sibB unchanged
    auto result = db.verify(child2Path, {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"c2-val", {}}, result->value, state.symbols);
}

TEST_F(PerSiblingInvalidationTest, PerSiblingRecovery_MultipleHistorical)
{
    // Record 3 versions of a child's trace (v1, v2, v3) with per-sibling deps.
    // Set sibling to match v1 → recovery should find v1 specifically.
    ScopedEnvVar env("NIX_PS_MH", "val1");

    auto db = makeDb();

    auto sibPath = makePath({"parent", "sib"});
    auto childPath = makePath({"parent", "child"});

    // v1: sib with val1
    db.record(sibPath, string_t{"sib-v1", {}},
              {makeEnvVarDep("NIX_PS_MH", "val1")}, false);
    auto sibHashV1 = db.getCurrentTraceHash(sibPath);
    ASSERT_TRUE(sibHashV1.has_value());
    db.record(childPath, string_t{"child-v1", {}},
              {makeParentContextDep(toDepKey(sibPath), *sibHashV1)}, false);

    // v2: sib with val2
    setenv("NIX_PS_MH", "val2", 1);
    db.record(sibPath, string_t{"sib-v2", {}},
              {makeEnvVarDep("NIX_PS_MH", "val2")}, false);
    auto sibHashV2 = db.getCurrentTraceHash(sibPath);
    ASSERT_TRUE(sibHashV2.has_value());
    db.record(childPath, string_t{"child-v2", {}},
              {makeParentContextDep(toDepKey(sibPath), *sibHashV2)}, false);

    // v3: sib with val3
    setenv("NIX_PS_MH", "val3", 1);
    db.record(sibPath, string_t{"sib-v3", {}},
              {makeEnvVarDep("NIX_PS_MH", "val3")}, false);
    auto sibHashV3 = db.getCurrentTraceHash(sibPath);
    ASSERT_TRUE(sibHashV3.has_value());
    db.record(childPath, string_t{"child-v3", {}},
              {makeParentContextDep(toDepKey(sibPath), *sibHashV3)}, false);

    // Revert sib to v1
    setenv("NIX_PS_MH", "val1", 1);
    db.record(sibPath, string_t{"sib-v1", {}},
              {makeEnvVarDep("NIX_PS_MH", "val1")}, false);

    db.clearSessionCaches();

    // Recovery should find child-v1 (matching sibHashV1)
    auto result = db.verify(childPath, {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-v1", {}}, result->value, state.symbols);
}

TEST_F(PerSiblingInvalidationTest, SiblingIdenticalResult_DifferentDeps_DifferentTraceHash)
{
    // Two sibling versions produce the same result string but have different
    // deps. This means different trace hashes. Per-sibling dep should detect
    // the change even though the result value is identical.
    ScopedEnvVar envV1("NIX_PS_ID", "v1-dep");

    auto db = makeDb();

    auto sibPath = makePath({"parent", "sib"});
    auto childPath = makePath({"parent", "child"});

    // Record sib v1: result "same-val", dep on NIX_PS_ID=v1-dep
    db.record(sibPath, string_t{"same-val", {}},
              {makeEnvVarDep("NIX_PS_ID", "v1-dep")}, false);
    auto sibHashV1 = db.getCurrentTraceHash(sibPath);
    ASSERT_TRUE(sibHashV1.has_value());

    // Record child with per-sibling dep on sib v1
    db.record(childPath, string_t{"child-val", {}},
              {makeParentContextDep(toDepKey(sibPath), *sibHashV1)}, false);

    // Re-record sib with SAME result but DIFFERENT dep → different trace hash
    setenv("NIX_PS_ID", "v2-dep", 1);
    db.record(sibPath, string_t{"same-val", {}},
              {makeEnvVarDep("NIX_PS_ID", "v2-dep")}, false);
    auto sibHashV2 = db.getCurrentTraceHash(sibPath);
    ASSERT_TRUE(sibHashV2.has_value());

    // Trace hashes should differ (deps differ, even though result is same)
    EXPECT_NE(std::memcmp(sibHashV1->hash, sibHashV2->hash, 32), 0)
        << "Sibling trace hashes should differ when deps differ";

    db.clearSessionCaches();

    // Child should fail — sib's trace hash changed
    auto result = db.verify(childPath, {}, state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(PerSiblingInvalidationTest, EmptyStringAttrPath_EdgeCase)
{
    // Test with an empty-string attr path component. This is an edge case
    // that tests the null-byte / tab conversion boundary.
    auto db = makeDb();

    auto sibPath = makePath({"", "sib"});    // empty first component
    auto childPath = makePath({"", "child"});

    db.record(sibPath, string_t{"sib-val", {}}, {}, false);
    auto sibHash = db.getCurrentTraceHash(sibPath);
    ASSERT_TRUE(sibHash.has_value());

    db.record(childPath, string_t{"child-val", {}},
              {makeParentContextDep(toDepKey(sibPath), *sibHash)}, false);

    db.clearSessionCaches();

    auto result = db.verify(childPath, {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-val", {}}, result->value, state.symbols);
}

TEST_F(PerSiblingInvalidationTest, PerSiblingDepOnFailedSibling)
{
    // A per-sibling dep can reference a sibling whose evaluation failed
    // (failed_t result). The trace hash should still work for verification
    // since failed_t still gets a trace recorded.
    ScopedEnvVar env("NIX_PS_FAIL", "val");

    auto db = makeDb();

    auto sibPath = makePath({"parent", "sib"});
    auto childPath = makePath({"parent", "child"});

    // Record sibling as failed
    db.record(sibPath, failed_t{},
              {makeEnvVarDep("NIX_PS_FAIL", "val")}, false);
    auto sibHash = db.getCurrentTraceHash(sibPath);
    ASSERT_TRUE(sibHash.has_value());

    // Record child with per-sibling dep on failed sibling
    db.record(childPath, string_t{"child-val", {}},
              {makeParentContextDep(toDepKey(sibPath), *sibHash)}, false);

    db.clearSessionCaches();

    // Verify passes — failed sibling's trace hash unchanged
    auto result = db.verify(childPath, {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-val", {}}, result->value, state.symbols);
}

TEST_F(PerSiblingInvalidationTest, PerSiblingDep_ChildIsAlsoSiblingOfAnotherChild)
{
    // Child A depends on child B as a sibling, and child B depends on child C
    // as a sibling. This tests that transitive per-sibling chains work correctly.
    ScopedEnvVar envC("NIX_PS_CHAIN_C", "c-val");

    auto db = makeDb();

    auto cPath = makePath({"parent", "c"});
    auto bPath = makePath({"parent", "b"});
    auto aPath = makePath({"parent", "a"});

    // Record C (leaf)
    db.record(cPath, string_t{"c-result", {}},
              {makeEnvVarDep("NIX_PS_CHAIN_C", "c-val")}, false);
    auto cHash = db.getCurrentTraceHash(cPath);
    ASSERT_TRUE(cHash.has_value());

    // Record B with per-sibling dep on C
    db.record(bPath, string_t{"b-result", {}},
              {makeParentContextDep(toDepKey(cPath), *cHash)}, false);
    auto bHash = db.getCurrentTraceHash(bPath);
    ASSERT_TRUE(bHash.has_value());

    // Record A with per-sibling dep on B
    db.record(aPath, string_t{"a-result", {}},
              {makeParentContextDep(toDepKey(bPath), *bHash)}, false);

    db.clearSessionCaches();

    // All pass when nothing changed
    EXPECT_TRUE(db.verify(aPath, {}, state).has_value());
    EXPECT_TRUE(db.verify(bPath, {}, state).has_value());
    EXPECT_TRUE(db.verify(cPath, {}, state).has_value());

    // Change C → B's per-sibling dep on C fails → B gets new trace hash →
    // A's per-sibling dep on B fails
    setenv("NIX_PS_CHAIN_C", "c-val-new", 1);
    db.record(cPath, string_t{"c-new", {}},
              {makeEnvVarDep("NIX_PS_CHAIN_C", "c-val-new")}, false);

    // Re-record B (since C changed, B would re-evaluate in real system)
    auto cHashNew = db.getCurrentTraceHash(cPath);
    ASSERT_TRUE(cHashNew.has_value());
    db.record(bPath, string_t{"b-new", {}},
              {makeParentContextDep(toDepKey(cPath), *cHashNew)}, false);

    db.clearSessionCaches();

    // A's per-sibling dep on B should now fail (B's trace hash changed)
    EXPECT_FALSE(db.verify(aPath, {}, state).has_value());
}

// ── Soundness gap: parent-mediated value changes ─────────────────────

/**
 * DISABLED test documenting a known soundness gap.
 *
 * Scenario: A parent overlay changes a child's definition (modifies the
 * Nix expression that produces the child's value) without changing any
 * file the child reads or any sibling the child accesses. With per-sibling
 * ParentContext deps, the child's trace incorrectly validates because:
 * - Per-sibling deps only track trace hashes of accessed siblings
 * - The overlay change doesn't affect any file deps (it's a Nix-level change)
 * - No accessed sibling's trace hash changed
 *
 * This test simulates the scenario: child has per-sibling dep on sibA.
 * Parent overlay changes the child's result (re-records child with a
 * different value and same deps). Without re-recording the sibling,
 * the child's old trace still validates because sibA's hash is unchanged.
 *
 * The EXPECT_FALSE at the end is what SHOULD happen (child should be
 * invalidated). The DISABLED prefix means gtest skips this test. When
 * this soundness gap is fixed, remove DISABLED_ and flip EXPECT_FALSE
 * to EXPECT_TRUE or adjust accordingly.
 *
 * See design.md §9.8 for full discussion.
 */
TEST_F(PerSiblingInvalidationTest, DISABLED_ParentMediatedValueChange_SoundnessGap)
{
    ScopedEnvVar envA("NIX_PS_PMV_A", "val-a");

    auto db = makeDb();

    auto sibAPath = makePath({"parent", "sibA"});
    auto childPath = makePath({"parent", "child"});

    // Record sibling A
    db.record(sibAPath, string_t{"sibA-val", {}},
              {makeEnvVarDep("NIX_PS_PMV_A", "val-a")}, false);
    auto sibAHash = db.getCurrentTraceHash(sibAPath);
    ASSERT_TRUE(sibAHash.has_value());

    // Record child with per-sibling dep on sibA, result = "original"
    db.record(childPath, string_t{"original", {}},
              {makeParentContextDep(toDepKey(sibAPath), *sibAHash)}, false);

    db.clearSessionCaches();

    // Verify passes (sibA unchanged) — returns "original"
    auto result1 = db.verify(childPath, {}, state);
    ASSERT_TRUE(result1.has_value());
    assertCachedResultEquals(string_t{"original", {}}, result1->value, state.symbols);

    // Now a parent overlay changes the child's definition. In real Nix, this
    // would mean the parent's expression was modified (e.g., an overlay applies
    // a patch to the child's meta). We simulate by re-recording the child with
    // a DIFFERENT result but the SAME per-sibling dep (sibA unchanged).
    db.record(childPath, string_t{"modified-by-overlay", {}},
              {makeParentContextDep(toDepKey(sibAPath), *sibAHash)}, false);

    db.clearSessionCaches();

    // BUG: The old trace for "original" is still in TraceHistory. Since sibA's
    // trace hash hasn't changed, the per-sibling dep on sibA still passes.
    // The verify path returns the CURRENT trace's result ("modified-by-overlay"),
    // not the old one. But the scenario we're testing is: what if we DON'T
    // re-record the child? The child's per-sibling dep passes, so the OLD
    // result would be served.
    //
    // To properly test this, we'd need to simulate the scenario where the
    // parent overlay changes the child's definition but the evaluator doesn't
    // know about it (no re-recording). This can't happen with TraceStore alone
    // — it requires the full TraceCache + evaluator integration.
    //
    // For now, this test documents the gap. The real scenario is:
    // 1. Cold eval: child evaluates to "original", trace recorded
    // 2. Parent overlay applied (Nix-level change, no file change)
    // 3. Warm eval: child's per-sibling deps pass, old "original" served
    //    instead of re-evaluating to get "modified-by-overlay"
    //
    // This EXPECT_FALSE documents what SHOULD happen: verification should
    // fail because the parent's definition of this child changed. Currently
    // it would pass (soundness gap).
    auto result2 = db.verify(childPath, {}, state);
    // With the soundness gap, this would be TRUE (old trace validates).
    // When fixed, this should be FALSE (parent change detected).
    // For now, we just document the gap exists.
    EXPECT_FALSE(result2.has_value()) << "Soundness gap: parent-mediated value "
        "change not detected by per-sibling ParentContext deps. "
        "See design.md §9.8.";
}

} // namespace nix::eval_trace
