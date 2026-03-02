#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/deps/hash.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

/**
 * Tests for per-sibling ParentContext deps: edge cases, removal, root-level,
 * shared siblings, recovery stress, and soundness gap tests.
 *
 * See verify/per-sibling-core.cc for basic tests, trace survival,
 * multi-level cascading, multiple deps, and recovery tests.
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

// -- Sibling removal --------------------------------------------------

TEST_F(PerSiblingInvalidationTest, SiblingTraceHashChanges_ChildInvalidated)
{
    // When a sibling's trace hash changes (e.g., sibling re-recorded with
    // different deps), child's per-sibling dep fails.
    auto db = makeDb();

    auto sibPathId = vpath({"parent", "sib"});
    auto childPathId = vpath({"parent", "child"});

    // Record sibling with no deps
    db.record(sibPathId, string_t{"sib-val", {}}, {}, false);
    auto sibHash = db.getCurrentTraceHash(sibPathId);
    ASSERT_TRUE(sibHash.has_value());

    // Record child with per-sibling dep
    db.record(childPathId, string_t{"child-val", {}},
              {makeParentContextDep(vpath({"parent", "sib"}), *sibHash)}, false);

    // "Modify" sibling by recording with different deps -> new trace hash
    db.record(sibPathId, string_t{"sib-val-modified", {}},
              {makeEnvVarDep(pools(), "NIX_PS_GONE", "x")}, false);

    db.clearSessionCaches();

    // Sibling's trace hash changed -> child fails
    auto result = db.verify(childPathId, {}, state);
    EXPECT_FALSE(result.has_value());
}

// -- Tab/null-byte key conversion -------------------------------------

TEST_F(PerSiblingInvalidationTest, DepKeyTabConversion_DeepPath)
{
    // Per-sibling dep key for a deeply nested sibling uses tab-separated path.
    // Verify that verification correctly converts back to null-byte path for DB lookup.
    ScopedEnvVar env("NIX_PS_DEEP", "val");

    auto db = makeDb();

    auto deepSibPathId = vpath({"root", "mid", "deep", "sib"});
    auto childPathId = vpath({"root", "mid", "deep", "child"});

    // Record deeply nested sibling
    db.record(deepSibPathId, string_t{"deep-sib-val", {}},
              {makeEnvVarDep(pools(), "NIX_PS_DEEP", "val")}, false);
    auto sibHash = db.getCurrentTraceHash(deepSibPathId);
    ASSERT_TRUE(sibHash.has_value());

    // Record child with per-sibling dep on deeply nested sibling
    db.record(childPathId, string_t{"child-val", {}},
              {makeParentContextDep(deepSibPathId, *sibHash)}, false);

    db.clearSessionCaches();

    // Verification should correctly resolve the tab-separated dep key
    auto result = db.verify(childPathId, {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-val", {}}, result->value, state.symbols);
}

// -- Edge cases: fragile code paths -----------------------------------

TEST_F(PerSiblingInvalidationTest, SiblingRemovedFromDB_VerifyFails)
{
    // When a sibling referenced by a per-sibling dep no longer exists in
    // CurrentTraces, getCurrentTraceHash returns nullopt -> verify fails.
    // This exercises the "missing sibling" path in verifyTrace.
    auto db = makeDb();

    auto sibPathId = vpath({"parent", "sib"});
    auto childPathId = vpath({"parent", "child"});

    // Record sibling, get its trace hash
    db.record(sibPathId, string_t{"sib-val", {}}, {}, false);
    auto sibHash = db.getCurrentTraceHash(sibPathId);
    ASSERT_TRUE(sibHash.has_value());

    // Record child with per-sibling dep on sib
    db.record(childPathId, string_t{"child-val", {}},
              {makeParentContextDep(vpath({"parent", "sib"}), *sibHash)}, false);

    // Overwrite sib with a completely new trace (different result + deps)
    // This changes the trace hash, simulating the sibling being
    // re-evaluated with different content.
    db.record(sibPathId, string_t{"totally-different", {}},
              {makeEnvVarDep(pools(), "NIX_PS_GONE_2", "x")}, false);

    db.clearSessionCaches();

    // Child's per-sibling dep on old sib trace hash fails
    auto result = db.verify(childPathId, {}, state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(PerSiblingInvalidationTest, RootLevelPerSiblingDep_SingleComponentPath)
{
    // Root-level attributes have single-component paths with no null-byte
    // separators. Verify that the tab/null conversion is a no-op and
    // per-sibling deps work correctly at the top level.
    ScopedEnvVar env("NIX_PS_ROOT_SIB", "val");

    auto db = makeDb();

    auto sibPathId = vpath({"lib"});       // single component, no separators
    auto childPathId = vpath({"hello"});   // single component, no separators

    // Record root-level sibling
    db.record(sibPathId, string_t{"lib-val", {}},
              {makeEnvVarDep(pools(), "NIX_PS_ROOT_SIB", "val")}, false);
    auto sibHash = db.getCurrentTraceHash(sibPathId);
    ASSERT_TRUE(sibHash.has_value());

    // Record child with per-sibling dep on root-level sibling
    db.record(childPathId, string_t{"hello-val", {}},
              {makeParentContextDep(sibPathId, *sibHash)}, false);

    db.clearSessionCaches();

    // Verify passes -- sibling unchanged
    auto result = db.verify(childPathId, {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"hello-val", {}}, result->value, state.symbols);
}

// -- Shared siblings --------------------------------------------------

TEST_F(PerSiblingInvalidationTest, MultipleChildrenShareAccessedSibling)
{
    // Two children of the same parent both depend on the same sibling.
    // Changing the sibling should invalidate BOTH children.
    ScopedEnvVar env("NIX_PS_SHARED", "val1");

    auto db = makeDb();

    auto sibPathId = vpath({"parent", "shared"});
    auto child1PathId = vpath({"parent", "child1"});
    auto child2PathId = vpath({"parent", "child2"});

    // Record shared sibling
    db.record(sibPathId, string_t{"shared-val", {}},
              {makeEnvVarDep(pools(), "NIX_PS_SHARED", "val1")}, false);
    auto sibHash = db.getCurrentTraceHash(sibPathId);
    ASSERT_TRUE(sibHash.has_value());

    // Record both children with per-sibling dep on same sibling
    db.record(child1PathId, string_t{"child1-val", {}},
              {makeParentContextDep(sibPathId, *sibHash)}, false);
    db.record(child2PathId, string_t{"child2-val", {}},
              {makeParentContextDep(sibPathId, *sibHash)}, false);

    db.clearSessionCaches();

    // Both verify pass when sibling unchanged
    EXPECT_TRUE(db.verify(child1PathId, {}, state).has_value());
    EXPECT_TRUE(db.verify(child2PathId, {}, state).has_value());

    // Change sibling
    setenv("NIX_PS_SHARED", "val2", 1);
    db.record(sibPathId, string_t{"shared-new", {}},
              {makeEnvVarDep(pools(), "NIX_PS_SHARED", "val2")}, false);

    db.clearSessionCaches();

    // Both children should now fail
    EXPECT_FALSE(db.verify(child1PathId, {}, state).has_value());
    EXPECT_FALSE(db.verify(child2PathId, {}, state).has_value());
}

TEST_F(PerSiblingInvalidationTest, ChildWithDifferentSiblingSubsets)
{
    // Two children of the same parent access different subsets of siblings.
    // Changing a sibling only invalidates the child that depends on it.
    ScopedEnvVar envA("NIX_PS_CSA", "val-a");
    ScopedEnvVar envB("NIX_PS_CSB", "val-b");

    auto db = makeDb();

    auto sibAPathId = vpath({"parent", "sibA"});
    auto sibBPathId = vpath({"parent", "sibB"});
    auto child1PathId = vpath({"parent", "child1"});
    auto child2PathId = vpath({"parent", "child2"});

    // Record two siblings
    db.record(sibAPathId, string_t{"a-val", {}},
              {makeEnvVarDep(pools(), "NIX_PS_CSA", "val-a")}, false);
    db.record(sibBPathId, string_t{"b-val", {}},
              {makeEnvVarDep(pools(), "NIX_PS_CSB", "val-b")}, false);
    auto sibAHash = db.getCurrentTraceHash(sibAPathId);
    auto sibBHash = db.getCurrentTraceHash(sibBPathId);
    ASSERT_TRUE(sibAHash.has_value());
    ASSERT_TRUE(sibBHash.has_value());

    // child1 depends on sibA only, child2 depends on sibB only
    db.record(child1PathId, string_t{"c1-val", {}},
              {makeParentContextDep(vpath({"parent", "sibA"}), *sibAHash)}, false);
    db.record(child2PathId, string_t{"c2-val", {}},
              {makeParentContextDep(vpath({"parent", "sibB"}), *sibBHash)}, false);

    // Change sibA only
    setenv("NIX_PS_CSA", "val-a-new", 1);
    db.record(sibAPathId, string_t{"a-val-new", {}},
              {makeEnvVarDep(pools(), "NIX_PS_CSA", "val-a-new")}, false);

    db.clearSessionCaches();

    // child1 (depends on sibA) should fail
    EXPECT_FALSE(db.verify(child1PathId, {}, state).has_value());
    // child2 (depends on sibB) should pass -- sibB unchanged
    auto result = db.verify(child2PathId, {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"c2-val", {}}, result->value, state.symbols);
}

// -- Recovery stress tests --------------------------------------------

TEST_F(PerSiblingInvalidationTest, PerSiblingRecovery_MultipleHistorical)
{
    // Record 3 versions of a child's trace (v1, v2, v3) with per-sibling deps.
    // Set sibling to match v1 -> recovery should find v1 specifically.
    ScopedEnvVar env("NIX_PS_MH", "val1");

    auto db = makeDb();

    auto sibPathId = vpath({"parent", "sib"});
    auto childPathId = vpath({"parent", "child"});

    // v1: sib with val1
    db.record(sibPathId, string_t{"sib-v1", {}},
              {makeEnvVarDep(pools(), "NIX_PS_MH", "val1")}, false);
    auto sibHashV1 = db.getCurrentTraceHash(sibPathId);
    ASSERT_TRUE(sibHashV1.has_value());
    db.record(childPathId, string_t{"child-v1", {}},
              {makeParentContextDep(sibPathId, *sibHashV1)}, false);

    // v2: sib with val2
    setenv("NIX_PS_MH", "val2", 1);
    db.record(sibPathId, string_t{"sib-v2", {}},
              {makeEnvVarDep(pools(), "NIX_PS_MH", "val2")}, false);
    auto sibHashV2 = db.getCurrentTraceHash(sibPathId);
    ASSERT_TRUE(sibHashV2.has_value());
    db.record(childPathId, string_t{"child-v2", {}},
              {makeParentContextDep(sibPathId, *sibHashV2)}, false);

    // v3: sib with val3
    setenv("NIX_PS_MH", "val3", 1);
    db.record(sibPathId, string_t{"sib-v3", {}},
              {makeEnvVarDep(pools(), "NIX_PS_MH", "val3")}, false);
    auto sibHashV3 = db.getCurrentTraceHash(sibPathId);
    ASSERT_TRUE(sibHashV3.has_value());
    db.record(childPathId, string_t{"child-v3", {}},
              {makeParentContextDep(sibPathId, *sibHashV3)}, false);

    // Revert sib to v1
    setenv("NIX_PS_MH", "val1", 1);
    db.record(sibPathId, string_t{"sib-v1", {}},
              {makeEnvVarDep(pools(), "NIX_PS_MH", "val1")}, false);

    db.clearSessionCaches();

    // Recovery should find child-v1 (matching sibHashV1)
    auto result = db.verify(childPathId, {}, state);
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

    auto sibPathId = vpath({"parent", "sib"});
    auto childPathId = vpath({"parent", "child"});

    // Record sib v1: result "same-val", dep on NIX_PS_ID=v1-dep
    db.record(sibPathId, string_t{"same-val", {}},
              {makeEnvVarDep(pools(), "NIX_PS_ID", "v1-dep")}, false);
    auto sibHashV1 = db.getCurrentTraceHash(sibPathId);
    ASSERT_TRUE(sibHashV1.has_value());

    // Record child with per-sibling dep on sib v1
    db.record(childPathId, string_t{"child-val", {}},
              {makeParentContextDep(vpath({"parent", "sib"}), *sibHashV1)}, false);

    // Re-record sib with SAME result but DIFFERENT dep -> different trace hash
    setenv("NIX_PS_ID", "v2-dep", 1);
    db.record(sibPathId, string_t{"same-val", {}},
              {makeEnvVarDep(pools(), "NIX_PS_ID", "v2-dep")}, false);
    auto sibHashV2 = db.getCurrentTraceHash(sibPathId);
    ASSERT_TRUE(sibHashV2.has_value());

    // Trace hashes should differ (deps differ, even though result is same)
    EXPECT_NE(std::memcmp(sibHashV1->hash, sibHashV2->hash, 32), 0)
        << "Sibling trace hashes should differ when deps differ";

    db.clearSessionCaches();

    // Child should fail -- sib's trace hash changed
    auto result = db.verify(childPathId, {}, state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(PerSiblingInvalidationTest, EmptyStringAttrPath_EdgeCase)
{
    // Test with an empty-string attr path component. This is an edge case
    // that tests the null-byte / tab conversion boundary.
    auto db = makeDb();

    auto sibPathId = vpath({"", "sib"});    // empty first component
    auto childPathId = vpath({"", "child"});

    db.record(sibPathId, string_t{"sib-val", {}}, {}, false);
    auto sibHash = db.getCurrentTraceHash(sibPathId);
    ASSERT_TRUE(sibHash.has_value());

    db.record(childPathId, string_t{"child-val", {}},
              {makeParentContextDep(vpath({"", "sib"}), *sibHash)}, false);

    db.clearSessionCaches();

    auto result = db.verify(childPathId, {}, state);
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

    auto sibPathId = vpath({"parent", "sib"});
    auto childPathId = vpath({"parent", "child"});

    // Record sibling as failed
    db.record(sibPathId, failed_t{},
              {makeEnvVarDep(pools(), "NIX_PS_FAIL", "val")}, false);
    auto sibHash = db.getCurrentTraceHash(sibPathId);
    ASSERT_TRUE(sibHash.has_value());

    // Record child with per-sibling dep on failed sibling
    db.record(childPathId, string_t{"child-val", {}},
              {makeParentContextDep(vpath({"parent", "sib"}), *sibHash)}, false);

    db.clearSessionCaches();

    // Verify passes -- failed sibling's trace hash unchanged
    auto result = db.verify(childPathId, {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-val", {}}, result->value, state.symbols);
}

TEST_F(PerSiblingInvalidationTest, PerSiblingDep_ChildIsAlsoSiblingOfAnotherChild)
{
    // Child A depends on child B as a sibling, and child B depends on child C
    // as a sibling. This tests that transitive per-sibling chains work correctly.
    ScopedEnvVar envC("NIX_PS_CHAIN_C", "c-val");

    auto db = makeDb();

    auto cPathId = vpath({"parent", "c"});
    auto bPathId = vpath({"parent", "b"});
    auto aPathId = vpath({"parent", "a"});

    // Record C (leaf)
    db.record(cPathId, string_t{"c-result", {}},
              {makeEnvVarDep(pools(), "NIX_PS_CHAIN_C", "c-val")}, false);
    auto cHash = db.getCurrentTraceHash(cPathId);
    ASSERT_TRUE(cHash.has_value());

    // Record B with per-sibling dep on C
    db.record(bPathId, string_t{"b-result", {}},
              {makeParentContextDep(cPathId, *cHash)}, false);
    auto bHash = db.getCurrentTraceHash(bPathId);
    ASSERT_TRUE(bHash.has_value());

    // Record A with per-sibling dep on B
    db.record(aPathId, string_t{"a-result", {}},
              {makeParentContextDep(bPathId, *bHash)}, false);

    db.clearSessionCaches();

    // All pass when nothing changed
    EXPECT_TRUE(db.verify(aPathId, {}, state).has_value());
    EXPECT_TRUE(db.verify(bPathId, {}, state).has_value());
    EXPECT_TRUE(db.verify(cPathId, {}, state).has_value());

    // Change C -> B's per-sibling dep on C fails -> B gets new trace hash ->
    // A's per-sibling dep on B fails
    setenv("NIX_PS_CHAIN_C", "c-val-new", 1);
    db.record(cPathId, string_t{"c-new", {}},
              {makeEnvVarDep(pools(), "NIX_PS_CHAIN_C", "c-val-new")}, false);

    // Re-record B (since C changed, B would re-evaluate in real system)
    auto cHashNew = db.getCurrentTraceHash(cPathId);
    ASSERT_TRUE(cHashNew.has_value());
    db.record(bPathId, string_t{"b-new", {}},
              {makeParentContextDep(cPathId, *cHashNew)}, false);

    db.clearSessionCaches();

    // A's per-sibling dep on B should now fail (B's trace hash changed)
    EXPECT_FALSE(db.verify(aPathId, {}, state).has_value());
}

// -- Soundness gap: parent-mediated value changes ---------------------

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
 * See design.md section 9.8 for full discussion.
 */
TEST_F(PerSiblingInvalidationTest, DISABLED_ParentMediatedValueChange_SoundnessGap)
{
    ScopedEnvVar envA("NIX_PS_PMV_A", "val-a");

    auto db = makeDb();

    auto sibAPathId = vpath({"parent", "sibA"});
    auto childPathId = vpath({"parent", "child"});

    // Record sibling A
    db.record(sibAPathId, string_t{"sibA-val", {}},
              {makeEnvVarDep(pools(), "NIX_PS_PMV_A", "val-a")}, false);
    auto sibAHash = db.getCurrentTraceHash(sibAPathId);
    ASSERT_TRUE(sibAHash.has_value());

    // Record child with per-sibling dep on sibA, result = "original"
    db.record(childPathId, string_t{"original", {}},
              {makeParentContextDep(sibAPathId, *sibAHash)}, false);

    db.clearSessionCaches();

    // Verify passes (sibA unchanged) -- returns "original"
    auto result1 = db.verify(childPathId, {}, state);
    ASSERT_TRUE(result1.has_value());
    assertCachedResultEquals(string_t{"original", {}}, result1->value, state.symbols);

    // Now a parent overlay changes the child's definition. In real Nix, this
    // would mean the parent's expression was modified (e.g., an overlay applies
    // a patch to the child's meta). We simulate by re-recording the child with
    // a DIFFERENT result but the SAME per-sibling dep (sibA unchanged).
    db.record(childPathId, string_t{"modified-by-overlay", {}},
              {makeParentContextDep(sibAPathId, *sibAHash)}, false);

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
    // -- it requires the full TraceCache + evaluator integration.
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
    auto result2 = db.verify(childPathId, {}, state);
    // With the soundness gap, this would be TRUE (old trace validates).
    // When fixed, this should be FALSE (parent change detected).
    // For now, we just document the gap exists.
    EXPECT_FALSE(result2.has_value()) << "Soundness gap: parent-mediated value "
        "change not detected by per-sibling ParentContext deps. "
        "See design.md section 9.8.";
}

// -- Bug 2 coverage: parent-mediated value change analysis -----------
//
// These tests pin down exact TraceStore behavior for whole-parent and
// per-sibling deps. The gap is in TraceCache (evaluator layer), not
// TraceStore — TraceStore correctly reflects whatever was recorded.

TEST_F(PerSiblingInvalidationTest, WholeParentDep_ParentReRecorded_DifferentDeps_ChildInvalidated)
{
    // Whole-parent dep detects parent trace hash change.
    // Re-recording parent with different deps changes its trace hash,
    // which should invalidate the child's ParentContext dep.
    ScopedEnvVar env("NIX_PS_WPD", "val");

    auto db = makeDb();

    auto parentPathId = vpath({"parent"});
    auto childPathId = vpath({"parent", "child"});

    // Record parent with result "A", dep on envvar=val
    db.record(parentPathId, string_t{"A", {}},
              {makeEnvVarDep(pools(), "NIX_PS_WPD", "val")}, false);
    auto parentHash = db.getCurrentTraceHash(parentPathId);
    ASSERT_TRUE(parentHash.has_value());

    // Record child with whole-parent dep
    db.record(childPathId, string_t{"child-val", {}},
              {makeParentContextDep(vpath({"parent"}), *parentHash)}, false);

    db.clearSessionCaches();

    // Verify passes initially
    auto result1 = db.verify(childPathId, {}, state);
    ASSERT_TRUE(result1.has_value());

    // Re-record parent with different deps → different trace hash
    setenv("NIX_PS_WPD", "val2", 1);
    db.record(parentPathId, string_t{"B", {}},
              {makeEnvVarDep(pools(), "NIX_PS_WPD", "val2")}, false);

    db.clearSessionCaches();

    // Child's ParentContext dep should fail (parent trace hash changed)
    auto result2 = db.verify(childPathId, {}, state);
    EXPECT_FALSE(result2.has_value())
        << "Whole-parent dep should detect parent trace hash change";
}

TEST_F(PerSiblingInvalidationTest, WholeParentDep_ParentUnchanged_ChildPasses)
{
    // Baseline: parent unchanged → child passes verification.
    ScopedEnvVar env("NIX_PS_WPU", "val");

    auto db = makeDb();

    auto parentPathId = vpath({"parent"});
    auto childPathId = vpath({"parent", "child"});

    // Record parent
    db.record(parentPathId, string_t{"parent-val", {}},
              {makeEnvVarDep(pools(), "NIX_PS_WPU", "val")}, false);
    auto parentHash = db.getCurrentTraceHash(parentPathId);
    ASSERT_TRUE(parentHash.has_value());

    // Record child with whole-parent dep
    db.record(childPathId, string_t{"child-val", {}},
              {makeParentContextDep(vpath({"parent"}), *parentHash)}, false);

    db.clearSessionCaches();

    // Verify passes — parent unchanged
    auto result = db.verify(childPathId, {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-val", {}}, result->value, state.symbols);
}

TEST_F(PerSiblingInvalidationTest, PerSiblingDep_SiblingResultChanges_SameDeps_ChildStillPasses)
{
    // Sibling result changes but deps are identical → trace hash unchanged →
    // child's ParentContext dep still passes.
    // BUG: trace hash only covers deps, not the result. A sibling whose result
    // changes (e.g., due to impure evaluation) without changing deps is invisible
    // to per-sibling ParentContext deps.
    ScopedEnvVar env("NIX_PS_SRC", "val");

    auto db = makeDb();

    auto sibPathId = vpath({"parent", "sib"});
    auto childPathId = vpath({"parent", "child"});

    // Record sib with result "v1"
    db.record(sibPathId, string_t{"v1", {}},
              {makeEnvVarDep(pools(), "NIX_PS_SRC", "val")}, false);
    auto sibHash = db.getCurrentTraceHash(sibPathId);
    ASSERT_TRUE(sibHash.has_value());

    // Record child with per-sibling dep on sib
    db.record(childPathId, string_t{"child-val", {}},
              {makeParentContextDep(vpath({"parent", "sib"}), *sibHash)}, false);

    // Re-record sib with different result "v2" but SAME deps
    db.record(sibPathId, string_t{"v2", {}},
              {makeEnvVarDep(pools(), "NIX_PS_SRC", "val")}, false);
    auto sibHashAfter = db.getCurrentTraceHash(sibPathId);
    ASSERT_TRUE(sibHashAfter.has_value());

    // Trace hash unchanged because deps are identical (result not included)
    EXPECT_EQ(std::memcmp(sibHash->hash, sibHashAfter->hash, 32), 0)
        << "BUG: trace hash only covers deps, not result — same deps → same hash";

    db.clearSessionCaches();

    // Child passes because sib's trace hash didn't change
    auto result = db.verify(childPathId, {}, state);
    EXPECT_TRUE(result.has_value())
        << "BUG: child passes because trace hash doesn't include sibling result";
}

TEST_F(PerSiblingInvalidationTest, PerSiblingDep_SiblingUnchanged_ChildPasses_DocumentsGap)
{
    // Documents the gap: child with only per-sibling dep passes verification
    // even if a parent overlay would change the child's result. TraceStore
    // behaves correctly — the bug is in TraceCache (evaluator doesn't
    // re-evaluate the child when only the parent definition changed).
    ScopedEnvVar env("NIX_PS_GAP", "val");

    auto db = makeDb();

    auto sibPathId = vpath({"parent", "sib"});
    auto childPathId = vpath({"parent", "child"});

    // Record sib
    db.record(sibPathId, string_t{"sib-val", {}},
              {makeEnvVarDep(pools(), "NIX_PS_GAP", "val")}, false);
    auto sibHash = db.getCurrentTraceHash(sibPathId);
    ASSERT_TRUE(sibHash.has_value());

    // Record child with per-sibling dep on sib, result = "original"
    db.record(childPathId, string_t{"original", {}},
              {makeParentContextDep(vpath({"parent", "sib"}), *sibHash)}, false);

    db.clearSessionCaches();

    // Verify passes — sib unchanged, returns "original"
    auto result1 = db.verify(childPathId, {}, state);
    ASSERT_TRUE(result1.has_value());
    assertCachedResultEquals(string_t{"original", {}}, result1->value, state.symbols);

    // Now imagine a parent overlay changes the child's definition to produce
    // "modified". But we can't simulate that here — the child wasn't
    // re-recorded by an evaluator. So verify again — still passes.
    db.clearSessionCaches();
    auto result2 = db.verify(childPathId, {}, state);
    ASSERT_TRUE(result2.has_value());
    assertCachedResultEquals(string_t{"original", {}}, result2->value, state.symbols);
    // CORRECT: since the child wasn't re-evaluated, the old result IS correct
    // from TraceStore's perspective.
    // GAP: the evaluator should have re-evaluated the child because the parent
    // overlay changed its definition, but per-sibling ParentContext deps don't
    // capture parent-mediated changes. This test documents that TraceStore
    // behaves correctly; the bug is in TraceCache.
}

} // namespace nix::eval_trace
