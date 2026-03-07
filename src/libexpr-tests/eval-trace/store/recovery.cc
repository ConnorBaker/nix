#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/expr/eval-trace/deps/hash.hh"

#include <algorithm>
#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── Constructive recovery tests (BSàlC: constructive traces) ─────────

TEST_F(TraceStoreTest, Phase1_StillWorks)
{
    // Phase 1 constructive recovery: same trace structure, different dep values, revert -> succeeds
    ScopedEnvVar env("NIX_P1_TEST", "value_A");

    auto db = makeDb();
    std::vector<Dep> depsA = {makeEnvVarDep(pools(), "NIX_P1_TEST", "value_A")};
    db.record(rootPath(), string_t{"result_A", {}}, depsA);

    // Change env var to value_B and record new trace
    setenv("NIX_P1_TEST", "value_B", 1);
    std::vector<Dep> depsB = {makeEnvVarDep(pools(), "NIX_P1_TEST", "value_B")};
    db.record(rootPath(), string_t{"result_B", {}}, depsB);

    // Revert to value_A -- Phase 1 constructive recovery should find the trace from first recording
    setenv("NIX_P1_TEST", "value_A", 1);
    db.clearSessionCaches();

    auto result = db.verify(rootPath(), {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"result_A", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, ChildSurvivesParentInvalidation_NoDeps)
{
    // Child with no deps survives parent invalidation (separated dep ownership).
    // After parent changes, child verifies independently because it has no deps.
    ScopedEnvVar env("NIX_P1_ROOT", "val1");

    auto db = makeDb();

    // Record root trace with val1
    std::vector<Dep> rootDeps1 = {makeEnvVarDep(pools(), "NIX_P1_ROOT", "val1")};
    db.record(rootPath(), string_t{"root1", {}}, rootDeps1);

    // Record child trace (no deps)
    db.record(vpath({"child"}), string_t{"child1", {}}, {});

    // Change root env var — root trace becomes invalid
    setenv("NIX_P1_ROOT", "val2", 1);
    db.clearSessionCaches();

    // Child has no deps → verifies immediately despite parent invalidation
    auto childResult = db.verify(vpath({"child"}), {}, state);
    ASSERT_TRUE(childResult.has_value());
    // Returns latest recorded result (child1 — only one was recorded for "child")
    assertCachedResultEquals(string_t{"child1", {}}, childResult->value, state.symbols);
}

TEST_F(TraceStoreTest, ChildSurvivesParentInvalidation_WithStableDeps)
{
    // Child with stable deps passes verification when parent changes.
    // With separated deps, child's trace_hash depends only on its own deps.
    ScopedEnvVar env1("NIX_P1W_ROOT", "rval1");
    ScopedEnvVar env2("NIX_P1W_CHILD", "cval");

    auto db = makeDb();

    // Record root trace with rval1
    std::vector<Dep> rootDeps1 = {makeEnvVarDep(pools(), "NIX_P1W_ROOT", "rval1")};
    db.record(rootPath(), string_t{"root1", {}}, rootDeps1);

    // Record child trace with stable dep (own dep only)
    std::vector<Dep> childDeps = {makeEnvVarDep(pools(), "NIX_P1W_CHILD", "cval")};
    db.record(vpath({"child"}), string_t{"child1", {}}, childDeps);

    // Change parent env var — parent trace becomes invalid
    setenv("NIX_P1W_ROOT", "rval2", 1);
    db.clearSessionCaches();

    // Child's own dep (NIX_P1W_CHILD=cval) is still valid → verify passes
    auto childResult = db.verify(vpath({"child"}), {}, state);
    ASSERT_TRUE(childResult.has_value());
    assertCachedResultEquals(string_t{"child1", {}}, childResult->value, state.symbols);
}

TEST_F(TraceStoreTest, IndependentVerification_TreeWithOwnDeps)
{
    // Each level has its own deps. With separated dep ownership, each level
    // verifies independently. Root and child recover via own deps.
    ScopedEnvVar env1("NIX_P1C_ROOT", "v1");
    ScopedEnvVar env2("NIX_P1C_CHILD", "cv1");

    auto db = makeDb();

    // Version 1: record traces for entire tree
    std::vector<Dep> rootDeps1 = {makeEnvVarDep(pools(), "NIX_P1C_ROOT", "v1")};
    std::vector<Dep> childDeps1 = {makeEnvVarDep(pools(), "NIX_P1C_CHILD", "cv1")};
    db.record(rootPath(), string_t{"r1", {}}, rootDeps1);
    db.record(vpath({"c1"}), string_t{"c1v1", {}}, childDeps1);
    db.record(vpath({"c1", "c2"}), string_t{"c2v1", {}}, {});

    // Version 2 — record new traces with changed deps at root and child
    setenv("NIX_P1C_ROOT", "v2", 1);
    setenv("NIX_P1C_CHILD", "cv2", 1);
    std::vector<Dep> rootDeps2 = {makeEnvVarDep(pools(), "NIX_P1C_ROOT", "v2")};
    std::vector<Dep> childDeps2 = {makeEnvVarDep(pools(), "NIX_P1C_CHILD", "cv2")};
    db.record(rootPath(), string_t{"r2", {}}, rootDeps2);
    db.record(vpath({"c1"}), string_t{"c1v2", {}}, childDeps2);
    db.record(vpath({"c1", "c2"}), string_t{"c2v2", {}}, {});

    // Revert to v1 — each level recovers via own deps
    setenv("NIX_P1C_ROOT", "v1", 1);
    setenv("NIX_P1C_CHILD", "cv1", 1);
    db.clearSessionCaches();

    // Root recovers via own dep (NIX_P1C_ROOT=v1)
    auto rootR = db.verify(rootPath(), {}, state);
    ASSERT_TRUE(rootR.has_value());
    assertCachedResultEquals(string_t{"r1", {}}, rootR->value, state.symbols);

    // Child recovers via own dep (NIX_P1C_CHILD=cv1)
    auto c1R = db.verify(vpath({"c1"}), {}, state);
    ASSERT_TRUE(c1R.has_value());
    assertCachedResultEquals(string_t{"c1v1", {}}, c1R->value, state.symbols);

    // Grandchild has no deps → verifies immediately
    auto c2R = db.verify(vpath({"c1", "c2"}), {}, state);
    ASSERT_TRUE(c2R.has_value());
}

TEST_F(TraceStoreTest, Phase3_DepStructMismatch)
{
    // Phase 3 constructive recovery: different trace structures between two recordings.
    // Phase 1 fails because dep keys differ. Phase 3 scans structural hash groups.
    ScopedEnvVar env1("NIX_P3_A", "aval");
    ScopedEnvVar env2("NIX_P3_B", "bval");

    auto db = makeDb();

    // First recording: trace with only dep A
    std::vector<Dep> deps1 = {makeEnvVarDep(pools(), "NIX_P3_A", "aval")};
    db.record(rootPath(), string_t{"result1", {}}, deps1);

    // Second recording: trace with deps A + B (different structural hash)
    std::vector<Dep> deps2 = {
        makeEnvVarDep(pools(), "NIX_P3_A", "aval"),
        makeEnvVarDep(pools(), "NIX_P3_B", "bval"),
    };
    db.record(rootPath(), string_t{"result2", {}}, deps2);

    // Now attribute points to trace with deps A+B.
    // Change B to invalidate that trace's deps.
    setenv("NIX_P3_B", "bval_new", 1);
    db.clearSessionCaches();

    // Phase 1 computes new trace hash for A+B (bval_new) -- no match.
    // Phase 3 scans structural hash groups, finds the group with only dep A,
    // recomputes current trace hash for A -> matches the first trace (BSàlC: constructive recovery).
    auto result = db.verify(rootPath(), {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"result1", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, Phase3_MultipleStructGroups)
{
    // 3 recordings with different trace structures. Phase 3 iterates structural hash groups.
    ScopedEnvVar env1("NIX_P3M_A", "a");
    ScopedEnvVar env2("NIX_P3M_B", "b");
    ScopedEnvVar env3("NIX_P3M_C", "c");

    auto db = makeDb();

    // Structural hash group 1: only A
    db.record(rootPath(), string_t{"r1", {}},
                  {makeEnvVarDep(pools(), "NIX_P3M_A", "a")});

    // Structural hash group 2: A + B
    db.record(rootPath(), string_t{"r2", {}},
                  {makeEnvVarDep(pools(), "NIX_P3M_A", "a"), makeEnvVarDep(pools(), "NIX_P3M_B", "b")});

    // Structural hash group 3: A + B + C (latest, in attribute entry)
    db.record(rootPath(), string_t{"r3", {}},
                  {makeEnvVarDep(pools(), "NIX_P3M_A", "a"), makeEnvVarDep(pools(), "NIX_P3M_B", "b"),
                   makeEnvVarDep(pools(), "NIX_P3M_C", "c")});

    // Change C -> invalidates structural hash group 3
    setenv("NIX_P3M_C", "c_new", 1);
    db.clearSessionCaches();

    auto result = db.verify(rootPath(), {}, state);
    // Should recover r1 or r2 (whichever structural group is verified first)
    ASSERT_TRUE(result.has_value());
    // Both r1 and r2 have valid dep hashes (A and B unchanged), either is acceptable
    auto & val = std::get<string_t>(result->value);
    EXPECT_TRUE(val.first == "r1" || val.first == "r2");
}

TEST_F(TraceStoreTest, Phase3_EmptyDeps)
{
    // Structural hash group with zero deps (empty trace)
    auto db = makeDb();

    db.record(rootPath(), string_t{"empty1", {}}, {});
    db.record(rootPath(), string_t{"empty2", {}},
                  {makeEnvVarDep(pools(), "NIX_P3E_X", "x")});

    // Invalidate X
    ScopedEnvVar env("NIX_P3E_X", "x_new");
    db.clearSessionCaches();

    auto result = db.verify(rootPath(), {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"empty1", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, Phase1_FallbackToPhase3)
{
    // No parent hint -- Phase 1 direct trace hash fails, Phase 3 structural recovery succeeds
    ScopedEnvVar env1("NIX_P1F_A", "a");
    ScopedEnvVar env2("NIX_P1F_B", "b");

    auto db = makeDb();

    // Two traces with different structural hashes
    db.record(rootPath(), string_t{"r1", {}},
                  {makeEnvVarDep(pools(), "NIX_P1F_A", "a")});
    db.record(rootPath(), string_t{"r2", {}},
                  {makeEnvVarDep(pools(), "NIX_P1F_A", "a"), makeEnvVarDep(pools(), "NIX_P1F_B", "b")});

    // Invalidate B
    setenv("NIX_P1F_B", "b_new", 1);
    db.clearSessionCaches();

    // Phase 1 fails (trace hash A+B with new B doesn't match). Phase 3 finds structural group with only A.
    auto result = db.verify(rootPath(), {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"r1", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, AllPhaseFail_Volatile)
{
    // Volatile dep (CurrentTime) -> immediate abort, no recovery possible (Shake: always-dirty)
    auto db = makeDb();

    std::vector<Dep> deps = {makeCurrentTimeDep(pools())};
    db.record(rootPath(), null_t{}, deps);

    auto result = db.verify(rootPath(), {}, state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(TraceStoreTest, RecoveryUpdatesAttribute)
{
    // After Phase 1 constructive recovery, next verify succeeds directly (attribute updated)
    ScopedEnvVar env("NIX_RUI_TEST", "val_A");

    auto db = makeDb();

    // Two trace recordings
    std::vector<Dep> depsA = {makeEnvVarDep(pools(), "NIX_RUI_TEST", "val_A")};
    db.record(rootPath(), string_t{"rA", {}}, depsA);

    setenv("NIX_RUI_TEST", "val_B", 1);
    std::vector<Dep> depsB = {makeEnvVarDep(pools(), "NIX_RUI_TEST", "val_B")};
    db.record(rootPath(), string_t{"rB", {}}, depsB);

    // Revert to A — constructive recovery should update attribute entry
    setenv("NIX_RUI_TEST", "val_A", 1);
    db.clearSessionCaches();

    auto r1 = db.verify(rootPath(), {}, state);
    ASSERT_TRUE(r1.has_value());

    // Second verification should succeed via direct lookup (no recovery needed)
    db.clearSessionCaches();
    auto r2 = db.verify(rootPath(), {}, state);
    ASSERT_TRUE(r2.has_value());
    assertCachedResultEquals(string_t{"rA", {}}, r2->value, state.symbols);
}

TEST_F(TraceStoreTest, Phase1_Then_Phase3_Cascade)
{
    // Phase 1 direct trace hash fails, Phase 3 structural recovery succeeds
    ScopedEnvVar env1("NIX_CASCADE_A", "a");
    ScopedEnvVar env2("NIX_CASCADE_B", "b");

    auto db = makeDb();

    // Trace with structural hash for only dep A (recovery target)
    db.record(vpath({"child"}), string_t{"target", {}},
                  {makeEnvVarDep(pools(), "NIX_CASCADE_A", "a")});

    // Trace with structural hash for deps A + B (latest, in attribute)
    db.record(vpath({"child"}), string_t{"latest", {}},
                  {makeEnvVarDep(pools(), "NIX_CASCADE_A", "a"), makeEnvVarDep(pools(), "NIX_CASCADE_B", "b")});

    // Invalidate B -> Phase 1 fails (trace hash A+B, B mismatches)
    // Phase 3 finds structural group with only A -> constructive recovery succeeds
    setenv("NIX_CASCADE_B", "b_new", 1);
    db.clearSessionCaches();

    auto result = db.verify(vpath({"child"}), {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"target", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, DeepChain_IndependentVerification)
{
    // Deep trace chain (depth 6): root -> c1 -> c2 -> c3 -> c4 -> c5
    // With separated deps, only root and c1 have deps. c2-c5 have no deps.
    // Reverting root/c1 deps triggers recovery for them, while c2-c5 verify immediately.
    ScopedEnvVar env1("NIX_DEEP_ROOT", "v1");
    ScopedEnvVar env2("NIX_DEEP_C1", "v1");

    auto db = makeDb();

    // Build attr paths
    auto c1Path = vpath({"c1"});
    auto c2Path = vpath({"c1", "c2"});
    auto c3Path = vpath({"c1", "c2", "c3"});
    auto c4Path = vpath({"c1", "c2", "c3", "c4"});
    auto c5Path = vpath({"c1", "c2", "c3", "c4", "c5"});

    // Version 1: record traces for chain with deps at root and c1
    std::vector<Dep> rootDeps1 = {makeEnvVarDep(pools(), "NIX_DEEP_ROOT", "v1")};
    std::vector<Dep> c1Deps1 = {makeEnvVarDep(pools(), "NIX_DEEP_C1", "v1")};
    db.record(rootPath(), string_t{"root_v1", {}}, rootDeps1);
    db.record(c1Path, string_t{"c1_v1", {}}, c1Deps1);
    db.record(c2Path, string_t{"c2_v1", {}}, {});
    db.record(c3Path, string_t{"c3_v1", {}}, {});
    db.record(c4Path, string_t{"c4_v1", {}}, {});
    db.record(c5Path, string_t{"c5_v1", {}}, {});

    // Version 2: record traces with different dep values
    setenv("NIX_DEEP_ROOT", "v2", 1);
    setenv("NIX_DEEP_C1", "v2", 1);
    std::vector<Dep> rootDeps2 = {makeEnvVarDep(pools(), "NIX_DEEP_ROOT", "v2")};
    std::vector<Dep> c1Deps2 = {makeEnvVarDep(pools(), "NIX_DEEP_C1", "v2")};
    db.record(rootPath(), string_t{"root_v2", {}}, rootDeps2);
    db.record(c1Path, string_t{"c1_v2", {}}, c1Deps2);
    db.record(c2Path, string_t{"c2_v2", {}}, {});
    db.record(c3Path, string_t{"c3_v2", {}}, {});
    db.record(c4Path, string_t{"c4_v2", {}}, {});
    db.record(c5Path, string_t{"c5_v2", {}}, {});

    // Revert to v1
    setenv("NIX_DEEP_ROOT", "v1", 1);
    setenv("NIX_DEEP_C1", "v1", 1);
    db.clearSessionCaches();

    // Root recovers via own dep hash match
    auto rootR = db.verify(rootPath(), {}, state);
    ASSERT_TRUE(rootR.has_value());
    assertCachedResultEquals(string_t{"root_v1", {}}, rootR->value, state.symbols);

    // c1 recovers via own dep hash match
    auto c1R = db.verify(c1Path, {}, state);
    ASSERT_TRUE(c1R.has_value());
    assertCachedResultEquals(string_t{"c1_v1", {}}, c1R->value, state.symbols);

    // c2-c5 have no deps → verify immediately (no recovery needed)
    ASSERT_TRUE(db.verify(c2Path, {}, state).has_value());
    ASSERT_TRUE(db.verify(c3Path, {}, state).has_value());
    ASSERT_TRUE(db.verify(c4Path, {}, state).has_value());
    ASSERT_TRUE(db.verify(c5Path, {}, state).has_value());
}

TEST_F(TraceStoreTest, RecoveryStress_10Versions)
{
    // Record 10 trace versions of the same attribute (each with different env var value).
    // Revert to version 1. Verify constructive recovery succeeds.
    ScopedEnvVar env("NIX_STRESS_VAR", "version_0");

    auto db = makeDb();

    // Record 10 trace versions
    for (int i = 0; i < 10; i++) {
        auto val = "version_" + std::to_string(i);
        setenv("NIX_STRESS_VAR", val.c_str(), 1);
        std::vector<Dep> deps = {makeEnvVarDep(pools(), "NIX_STRESS_VAR", val)};
        auto result = "result_" + std::to_string(i);
        db.record(rootPath(), string_t{result, {}}, deps);
    }

    // Revert to each version and verify constructive recovery
    for (int target = 0; target < 10; target++) {
        auto val = "version_" + std::to_string(target);
        setenv("NIX_STRESS_VAR", val.c_str(), 1);
        db.clearSessionCaches();

        auto result = db.verify(rootPath(), {}, state);
        ASSERT_TRUE(result.has_value()) << "Constructive recovery failed for version " << target;
        auto expected = "result_" + std::to_string(target);
        assertCachedResultEquals(string_t{expected, {}}, result->value, state.symbols);
    }
}

TEST_F(TraceStoreTest, RecoveryFailure_AllPhasesFail)
{
    // All recovery phases fail: attribute has dep on env var, env var changed to a value
    // never recorded. No constructive recovery candidate matches (BSàlC: no matching trace).
    ScopedEnvVar env("NIX_FAIL_VAR", "original");

    auto db = makeDb();

    std::vector<Dep> deps = {makeEnvVarDep(pools(), "NIX_FAIL_VAR", "original")};
    db.record(rootPath(), string_t{"old_result", {}}, deps);

    // Change to a NEVER-RECORDED value
    setenv("NIX_FAIL_VAR", "completely_new_value", 1);
    db.clearSessionCaches();

    auto result = db.verify(rootPath(), {}, state);
    EXPECT_FALSE(result.has_value());
}

// ── Fix 1: Sentinel hash tests (missing file deps) ──────────────────

TEST_F(TraceStoreTest, Recovery_MissingFileDep_StructuralVariant)
{
    // Record T1 with deps [Content(X)], record T2 with deps [Content(X), Content(Y)].
    // Delete Y. Verify → recovers T1 (structural variant without Y).
    TempTestFile fileX("content_X");
    TempTestFile fileY("content_Y");

    auto db = makeDb();

    // T1: only depends on X
    std::vector<Dep> deps1 = {makeContentDep(pools(), fileX.path.string(), "content_X")};
    db.record(rootPath(), string_t{"result_T1", {}}, deps1);

    // T2: depends on X + Y (latest, in attribute)
    std::vector<Dep> deps2 = {
        makeContentDep(pools(), fileX.path.string(), "content_X"),
        makeContentDep(pools(), fileY.path.string(), "content_Y"),
    };
    db.record(rootPath(), string_t{"result_T2", {}}, deps2);

    // Delete Y → T2's Content(Y) dep can't be verified normally
    std::filesystem::remove(fileY.path);
    getFSSourceAccessor()->invalidateCache(CanonPath(fileY.path.string()));
    StatHashStore::instance().clear();
    db.clearSessionCaches();

    // Recovery: sentinel for Y, structural variant finds T1 (no Y dep)
    auto result = db.verify(rootPath(), {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"result_T1", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, Recovery_MissingFile_DirectHashOnRevert)
{
    // Record T1 with [Content(X)="v1"]. Record T2 with [Content(X)="v2"].
    // Revert X to v1 content. Direct hash recovery succeeds (X matches v1).
    TempTestFile fileX("v1_content");

    auto db = makeDb();

    // T1: X has v1 content
    std::vector<Dep> deps1 = {makeContentDep(pools(), fileX.path.string(), "v1_content")};
    db.record(rootPath(), string_t{"result_v1", {}}, deps1);

    // T2: X has v2 content
    fileX.modify("v2_content");
    getFSSourceAccessor()->invalidateCache(CanonPath(fileX.path.string()));
    StatHashStore::instance().clear();
    std::vector<Dep> deps2 = {makeContentDep(pools(), fileX.path.string(), "v2_content")};
    db.record(rootPath(), string_t{"result_v2", {}}, deps2);

    // Revert X to v1
    fileX.modify("v1_content");
    getFSSourceAccessor()->invalidateCache(CanonPath(fileX.path.string()));
    StatHashStore::instance().clear();
    db.clearSessionCaches();

    auto result = db.verify(rootPath(), {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"result_v1", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, Recovery_SentinelDeterministic)
{
    // depHash("<missing>") computed twice must be equal.
    auto h1 = depHash("<missing>");
    auto h2 = depHash("<missing>");
    EXPECT_EQ(h1, h2);
}

TEST_F(TraceStoreTest, Recovery_MissingFile_AllDepsStillComputable)
{
    // Record trace with Content dep on file X. Delete X.
    // Recovery should still compute all deps (sentinel returned, not nullopt).
    // Verified by: recovery doesn't abort (allComputable = true), tries direct hash.
    TempTestFile fileX("some_content");

    auto db = makeDb();

    std::vector<Dep> deps = {makeContentDep(pools(), fileX.path.string(), "some_content")};
    db.record(rootPath(), string_t{"result", {}}, deps);

    // Also record a second version so recovery has something to try
    fileX.modify("other_content");
    getFSSourceAccessor()->invalidateCache(CanonPath(fileX.path.string()));
    StatHashStore::instance().clear();
    std::vector<Dep> deps2 = {makeContentDep(pools(), fileX.path.string(), "other_content")};
    db.record(rootPath(), string_t{"result2", {}}, deps2);

    // Delete X
    std::filesystem::remove(fileX.path);
    getFSSourceAccessor()->invalidateCache(CanonPath(fileX.path.string()));
    StatHashStore::instance().clear();
    db.clearSessionCaches();

    // Recovery attempts (sentinel hash won't match either v1 or v2, but
    // allComputable should be true so recovery runs without aborting).
    // No matching trace → verify returns nullopt, but the point is recovery
    // didn't abort early due to nullopt.
    auto result = db.verify(rootPath(), {}, state);
    // Both versions had the file → no structural variant without it → fails
    EXPECT_FALSE(result.has_value());
}

TEST_F(TraceStoreTest, Recovery_MissingFile_NoMatchingVariant)
{
    // Record ONE trace with Content(X). Delete X. Recovery fails
    // (sentinel hash ≠ stored, no variant without X).
    TempTestFile fileX("only_version");

    auto db = makeDb();

    std::vector<Dep> deps = {makeContentDep(pools(), fileX.path.string(), "only_version")};
    db.record(rootPath(), string_t{"result", {}}, deps);

    // Delete X
    std::filesystem::remove(fileX.path);
    getFSSourceAccessor()->invalidateCache(CanonPath(fileX.path.string()));
    StatHashStore::instance().clear();
    db.clearSessionCaches();

    auto result = db.verify(rootPath(), {}, state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(TraceStoreTest, Verify_MissingFile_ContentFails)
{
    // Record trace with Content(X). Delete X. verifyTrace fails
    // (sentinel ≠ stored hash), falls through to recovery.
    TempTestFile fileX("original_content");

    auto db = makeDb();

    std::vector<Dep> deps = {makeContentDep(pools(), fileX.path.string(), "original_content")};
    db.record(rootPath(), string_t{"result", {}}, deps);

    // Delete X → Content dep mismatches (sentinel vs original hash)
    std::filesystem::remove(fileX.path);
    getFSSourceAccessor()->invalidateCache(CanonPath(fileX.path.string()));
    StatHashStore::instance().clear();
    db.clearSessionCaches();

    // Should fail (only one trace, sentinel won't match stored content hash)
    auto result = db.verify(rootPath(), {}, state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(TraceStoreTest, Recovery_SentinelDiffersFromRealHash)
{
    // depHash("<missing>") must differ from depHash of any real content.
    auto sentinel = depHash("<missing>");
    auto real1 = depHash("hello");
    auto real2 = depHash("");
    auto real3 = depHash("some file content\n");
    EXPECT_NE(sentinel, real1);
    EXPECT_NE(sentinel, real2);
    EXPECT_NE(sentinel, real3);
}


} // namespace nix::eval_trace
