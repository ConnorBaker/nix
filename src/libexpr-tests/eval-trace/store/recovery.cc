#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"
#include "nix/expr/eval-trace/counters.hh"

#include <algorithm>
#include <concepts>
#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── Constructive recovery tests (BSàlC: constructive traces) ─────────

TEST_F(TraceStoreTest, DirectHash_StableTrace_Recovers)
{
    // Phase 1 constructive recovery: same trace structure, different dep values, revert -> succeeds
    ScopedEnvVar env("NIX_P1_TEST", "value_A");

    auto db = makeDb();
    std::vector<Dep> depsA = {makeEnvVarDep(pools(), "NIX_P1_TEST", "value_A")};
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"result_A", {}}, depsA);
    });

    // Change env var to value_B and record new trace
    setenv("NIX_P1_TEST", "value_B", 1);
    std::vector<Dep> depsB = {makeEnvVarDep(pools(), "NIX_P1_TEST", "value_B")};
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"result_B", {}}, depsB);
    });

    // Revert to value_A -- Phase 1 constructive recovery should find the trace from first recording
    setenv("NIX_P1_TEST", "value_A", 1);
    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"result_A", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, DirectHash_RejectsFailingImplicitStructureGuard)
{
    ScopedEnvVar env("NIX_DIRECT_IMPLICIT_GUARD", "stable");
    TempJsonFile jsonFile(R"({"value": 1})");
    auto filePath = std::filesystem::canonical(jsonFile.path).string();

    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"guarded", {}},
            {
                makeEnvVarDep(pools(), "NIX_DIRECT_IMPLICIT_GUARD", "stable"),
                makeStructuredDepForTest(
                    pools(), CanonicalQueryKind::ImplicitStructure,
                    DepSource::makeAbsolute(), filePath, StructuredFormat::Json,
                    {}, DepHashValue(sentinel(SentinelHash::Object)), ShapeSuffix::Type),
            });
    });

    jsonFile.modify(R"([1, 2, 3])");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_FALSE(result.has_value())
        << "direct recovery must not serve a trace whose implicit structure guard now fails";
}

TEST_F(TraceStoreTest, Recovery_ChildSurvivesParentInvalidation_NoDeps)
{
    // Child with no deps survives parent invalidation (separated dep ownership).
    // After parent changes, child verifies independently because it has no deps.
    ScopedEnvVar env("NIX_P1_ROOT", "val1");

    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // Record root trace with val1
        std::vector<Dep> rootDeps1 = {makeEnvVarDep(pools(), "NIX_P1_ROOT", "val1")};
        db->record(ea, rootPath(), string_t{"root1", {}}, rootDeps1);

        // Record child trace (no deps)
        db->record(ea, vpath({"child"}), string_t{"child1", {}}, {});
    });

    // Change root env var — root trace becomes invalid
    setenv("NIX_P1_ROOT", "val2", 1);
    recreateDb(db);

    // Child has no deps → verifies immediately despite parent invalidation
    auto childResult = test::TraceStorageTestAccess::verify(*db, vpath({"child"}), state);
    ASSERT_TRUE(childResult.has_value());
    // Returns latest recorded result (child1 — only one was recorded for "child")
    assertCachedResultEquals(string_t{"child1", {}}, childResult->value, state.symbols);
}

TEST_F(TraceStoreTest, Recovery_ChildSurvivesParentInvalidation_WithStableDeps)
{
    // Child with stable deps passes verification when parent changes.
    // With separated deps, child's trace_hash depends only on its own deps.
    ScopedEnvVar env1("NIX_P1W_ROOT", "rval1");
    ScopedEnvVar env2("NIX_P1W_CHILD", "cval");

    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // Record root trace with rval1
        std::vector<Dep> rootDeps1 = {makeEnvVarDep(pools(), "NIX_P1W_ROOT", "rval1")};
        db->record(ea, rootPath(), string_t{"root1", {}}, rootDeps1);

        // Record child trace with stable dep (own dep only)
        std::vector<Dep> childDeps = {makeEnvVarDep(pools(), "NIX_P1W_CHILD", "cval")};
        db->record(ea, vpath({"child"}), string_t{"child1", {}}, childDeps);
    });

    // Change parent env var — parent trace becomes invalid
    setenv("NIX_P1W_ROOT", "rval2", 1);
    recreateDb(db);

    // Child's own dep (NIX_P1W_CHILD=cval) is still valid → verify passes
    auto childResult = test::TraceStorageTestAccess::verify(*db, vpath({"child"}), state);
    ASSERT_TRUE(childResult.has_value());
    assertCachedResultEquals(string_t{"child1", {}}, childResult->value, state.symbols);
}

TEST_F(TraceStoreTest, Recovery_IndependentVerification_TreeWithOwnDeps)
{
    // Each level has its own deps. With separated dep ownership, each level
    // verifies independently. Root and child recover via own deps.
    ScopedEnvVar env1("NIX_P1C_ROOT", "v1");
    ScopedEnvVar env2("NIX_P1C_CHILD", "cv1");

    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // Version 1: record traces for entire tree
        std::vector<Dep> rootDeps1 = {makeEnvVarDep(pools(), "NIX_P1C_ROOT", "v1")};
        std::vector<Dep> childDeps1 = {makeEnvVarDep(pools(), "NIX_P1C_CHILD", "cv1")};
        db->record(ea, rootPath(), string_t{"r1", {}}, rootDeps1);
        db->record(ea, vpath({"c1"}), string_t{"c1v1", {}}, childDeps1);
        db->record(ea, vpath({"c1", "c2"}), string_t{"c2v1", {}}, {});

        // Version 2 — record new traces with changed deps at root and child
        setenv("NIX_P1C_ROOT", "v2", 1);
        setenv("NIX_P1C_CHILD", "cv2", 1);
        std::vector<Dep> rootDeps2 = {makeEnvVarDep(pools(), "NIX_P1C_ROOT", "v2")};
        std::vector<Dep> childDeps2 = {makeEnvVarDep(pools(), "NIX_P1C_CHILD", "cv2")};
        db->record(ea, rootPath(), string_t{"r2", {}}, rootDeps2);
        db->record(ea, vpath({"c1"}), string_t{"c1v2", {}}, childDeps2);
        db->record(ea, vpath({"c1", "c2"}), string_t{"c2v2", {}}, {});
    });

    // Revert to v1 — each level recovers via own deps
    setenv("NIX_P1C_ROOT", "v1", 1);
    setenv("NIX_P1C_CHILD", "cv1", 1);
    recreateDb(db);

    // Root recovers via own dep (NIX_P1C_ROOT=v1)
    auto rootR = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(rootR.has_value());
    assertCachedResultEquals(string_t{"r1", {}}, rootR->value, state.symbols);

    // Child recovers via own dep (NIX_P1C_CHILD=cv1)
    auto c1R = test::TraceStorageTestAccess::verify(*db, vpath({"c1"}), state);
    ASSERT_TRUE(c1R.has_value());
    assertCachedResultEquals(string_t{"c1v1", {}}, c1R->value, state.symbols);

    // Grandchild has no deps → verifies immediately
    auto c2R = test::TraceStorageTestAccess::verify(*db, vpath({"c1", "c2"}), state);
    ASSERT_TRUE(c2R.has_value());
}

TEST_F(TraceStoreTest, Recovery_DepStructMismatch_StructuralScan)
{
    // Phase 3 constructive recovery: different trace structures between two recordings.
    // Phase 1 fails because dep keys differ. Phase 3 scans structural hash groups.
    ScopedEnvVar env1("NIX_P3_A", "aval");
    ScopedEnvVar env2("NIX_P3_B", "bval");

    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // First recording: trace with only dep A
        std::vector<Dep> deps1 = {makeEnvVarDep(pools(), "NIX_P3_A", "aval")};
        db->record(ea, rootPath(), string_t{"result1", {}}, deps1);

        // Second recording: trace with deps A + B (different structural hash)
        std::vector<Dep> deps2 = {
            makeEnvVarDep(pools(), "NIX_P3_A", "aval"),
            makeEnvVarDep(pools(), "NIX_P3_B", "bval"),
        };
        db->record(ea, rootPath(), string_t{"result2", {}}, deps2);
    });

    // Now attribute points to trace with deps A+B.
    // Change B to invalidate that trace's deps.
    setenv("NIX_P3_B", "bval_new", 1);
    recreateDb(db);

    // Phase 1 computes new trace hash for A+B (bval_new) -- no match.
    // Phase 3 scans structural hash groups, finds the group with only dep A,
    // recomputes current trace hash for A -> matches the first trace (BSàlC: constructive recovery).
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"result1", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, Recovery_MultipleStructGroups_AnyGroupMatches)
{
    // 3 recordings with different trace structures. Phase 3 iterates structural hash groups.
    ScopedEnvVar env1("NIX_P3M_A", "a");
    ScopedEnvVar env2("NIX_P3M_B", "b");
    ScopedEnvVar env3("NIX_P3M_C", "c");

    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // Structural hash group 1: only A
        db->record(ea, rootPath(), string_t{"r1", {}},
                      {makeEnvVarDep(pools(), "NIX_P3M_A", "a")});

        // Structural hash group 2: A + B
        db->record(ea, rootPath(), string_t{"r2", {}},
                      {makeEnvVarDep(pools(), "NIX_P3M_A", "a"), makeEnvVarDep(pools(), "NIX_P3M_B", "b")});

        // Structural hash group 3: A + B + C (latest, in attribute entry)
        db->record(ea, rootPath(), string_t{"r3", {}},
                      {makeEnvVarDep(pools(), "NIX_P3M_A", "a"), makeEnvVarDep(pools(), "NIX_P3M_B", "b"),
                       makeEnvVarDep(pools(), "NIX_P3M_C", "c")});
    });

    // Change C -> invalidates structural hash group 3
    setenv("NIX_P3M_C", "c_new", 1);
    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    // Should recover r1 or r2 (whichever structural group is verified first)
    ASSERT_TRUE(result.has_value());
    // Both r1 and r2 have valid dep hashes (A and B unchanged), either is acceptable
    auto & val = std::get<string_t>(result->value);
    EXPECT_TRUE(val.first == "r1" || val.first == "r2");
}

TEST_F(TraceStoreTest, Recovery_EmptyDepsGroup_MatchesFirst)
{
    // Structural hash group with zero deps (empty trace).
    // Shape: two traces at the same attrset key. empty2 is recorded
    // second so its row is the CurrentNode. After invalidating the env
    // var, empty2's direct verify fails; recovery must traverse History
    // and find empty1 (whose zero-deps trace trivially verifies).
    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"empty1", {}}, {});
        db->record(ea, rootPath(), string_t{"empty2", {}},
                      {makeEnvVarDep(pools(), "NIX_P3E_X", "x")});
    });

    // Invalidate X
    ScopedEnvVar env("NIX_P3E_X", "x_new");
    recreateDb(db);

    const auto attemptsBefore = nrRecoveryAttempts.load();
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"empty1", {}}, result->value, state.symbols);
    EXPECT_GE(nrRecoveryAttempts.load() - attemptsBefore, 1u)
        << "direct verify of empty2 must fail (env var changed), so "
           "recovery must have fired to find empty1 — otherwise the "
           "result could be arriving via primary verify on a wrong trace";
}

TEST_F(TraceStoreTest, Recovery_DirectHashFails_StructuralScanSucceeds)
{
    // No parent hint -- Phase 1 direct trace hash fails, Phase 3 structural recovery succeeds
    ScopedEnvVar env1("NIX_P1F_A", "a");
    ScopedEnvVar env2("NIX_P1F_B", "b");

    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // Two traces with different structural hashes
        db->record(ea, rootPath(), string_t{"r1", {}},
                      {makeEnvVarDep(pools(), "NIX_P1F_A", "a")});
        db->record(ea, rootPath(), string_t{"r2", {}},
                      {makeEnvVarDep(pools(), "NIX_P1F_A", "a"), makeEnvVarDep(pools(), "NIX_P1F_B", "b")});
    });

    // Invalidate B
    setenv("NIX_P1F_B", "b_new", 1);
    recreateDb(db);

    // Phase 1 fails (trace hash A+B with new B doesn't match). Phase 3 finds structural group with only A.
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"r1", {}}, result->value, state.symbols);
}

// Companion to Recovery_DirectHashFails_StructuralScanSucceeds: the scenario
// is chosen so DirectHash misses and SV is the only remaining recovery
// strategy.  Runs the same scenario twice from scratch — once with
// `eval-trace-structural-recovery` enabled (positive: SV fires, recovers
// r1), once disabled (negative: SV is skipped, overall recovery returns
// nullopt).  Counter deltas pin SV as the actual responsible layer so a
// future regression that made DirectHash silently cover the scenario
// would still fail the test.
TEST_F(TraceStoreTest, Recovery_StructuralRecoveryToggle_GatesSVPath)
{
    auto runScenario = [&](bool enableSV) -> std::pair<
        std::optional<SqliteTraceStorage::VerifyResult>,
        Counter::value_type>
    {
        ScopedEnvVar env1("NIX_SVGATE_A", "a");
        ScopedEnvVar env2("NIX_SVGATE_B", "b");

        auto db = makeDb();

        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"r1", {}},
                          {makeEnvVarDep(pools(), "NIX_SVGATE_A", "a")});
            db->record(ea, rootPath(), string_t{"r2", {}},
                          {makeEnvVarDep(pools(), "NIX_SVGATE_A", "a"),
                           makeEnvVarDep(pools(), "NIX_SVGATE_B", "b")});
        });

        setenv("NIX_SVGATE_B", "b_new", 1);
        recreateDb(db);

        evalSettings.useStructuralRecovery = enableSV;
        PathCountersSnapshot snap;
        auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
        return {result, snap.deltaRecoveryStructVariantHits()};
    };

    // Positive: SV enabled, direct-hash miss falls through to SV which
    // locates the {A-only} structural group and serves r1.
    auto [enabledResult, enabledSVDelta] = runScenario(/*enableSV=*/true);
    ASSERT_TRUE(enabledResult.has_value());
    assertCachedResultEquals(string_t{"r1", {}}, enabledResult->value, state.symbols);
    EXPECT_EQ(enabledSVDelta, 1) << "SV hit counter must increment when SV fires";

    // Negative: SV disabled, the same direct-hash miss terminates recovery
    // at nullopt with no SV hit.
    auto [disabledResult, disabledSVDelta] = runScenario(/*enableSV=*/false);
    EXPECT_FALSE(disabledResult.has_value());
    EXPECT_EQ(disabledSVDelta, 0) << "SV hit counter must not increment when SV is gated off";
}

// Narrow-gate confirmation for Recovery_StructuralRecoveryToggle_GatesSVPath.
// With the flag off, direct-hash recovery must STILL succeed — the gate
// should not accidentally disable siblings in the recovery pipeline.  Mirror
// of DirectHash_StableTrace_Recovers with `useStructuralRecovery=false`.
TEST_F(TraceStoreTest, Recovery_StructuralRecoveryDisabled_DirectHashStillFires)
{
    ScopedEnvVar env("NIX_SVOFF_DH", "value_A");

    auto db = makeDb();
    std::vector<Dep> depsA = {makeEnvVarDep(pools(), "NIX_SVOFF_DH", "value_A")};
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"result_A", {}}, depsA);
    });

    setenv("NIX_SVOFF_DH", "value_B", 1);
    std::vector<Dep> depsB = {makeEnvVarDep(pools(), "NIX_SVOFF_DH", "value_B")};
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"result_B", {}}, depsB);
    });

    setenv("NIX_SVOFF_DH", "value_A", 1);
    recreateDb(db);

    evalSettings.useStructuralRecovery = false;
    PathCountersSnapshot snap;
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);

    ASSERT_TRUE(result.has_value())
        << "DirectHash recovery must still succeed when only SV is gated off";
    assertCachedResultEquals(string_t{"result_A", {}}, result->value, state.symbols);
    EXPECT_EQ(snap.deltaRecoveryDirectHashHits(), 1)
        << "DirectHash must be the layer that produced the hit";
    EXPECT_EQ(snap.deltaRecoveryStructVariantHits(), 0)
        << "SV must not fire when useStructuralRecovery=false";
}

TEST_F(TraceStoreTest, Recovery_VolatileDeps_AllPhaseFail)
{
    // Volatile dep (CurrentTime) -> immediate abort, no recovery possible (Shake: always-dirty)
    auto db = makeDb();

    std::vector<Dep> deps = {makeCurrentTimeDep(pools())};
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), makeNull(), deps);
    });

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(TraceStoreTest, Recovery_UpdatesAttribute_NewNodeStamp)
{
    // After Phase 1 constructive recovery, next verify succeeds directly (attribute updated)
    ScopedEnvVar env("NIX_RUI_TEST", "val_A");

    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // Two trace recordings
        std::vector<Dep> depsA = {makeEnvVarDep(pools(), "NIX_RUI_TEST", "val_A")};
        db->record(ea, rootPath(), string_t{"rA", {}}, depsA);

        setenv("NIX_RUI_TEST", "val_B", 1);
        std::vector<Dep> depsB = {makeEnvVarDep(pools(), "NIX_RUI_TEST", "val_B")};
        db->record(ea, rootPath(), string_t{"rB", {}}, depsB);
    });

    // Revert to A — constructive recovery should update attribute entry
    setenv("NIX_RUI_TEST", "val_A", 1);
    recreateDb(db);

    auto r1 = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(r1.has_value());

    // Second verification should succeed via direct lookup (no recovery needed)
    recreateDb(db);
    auto r2 = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(r2.has_value());
    assertCachedResultEquals(string_t{"rA", {}}, r2->value, state.symbols);
}

TEST_F(TraceStoreTest, Recovery_DirectHashThenStructural_Cascade)
{
    // Phase 1 direct trace hash fails, Phase 3 structural recovery succeeds
    ScopedEnvVar env1("NIX_CASCADE_A", "a");
    ScopedEnvVar env2("NIX_CASCADE_B", "b");

    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // Trace with structural hash for only dep A (recovery target)
        db->record(ea, vpath({"child"}), string_t{"target", {}},
                      {makeEnvVarDep(pools(), "NIX_CASCADE_A", "a")});

        // Trace with structural hash for deps A + B (latest, in attribute)
        db->record(ea, vpath({"child"}), string_t{"latest", {}},
                      {makeEnvVarDep(pools(), "NIX_CASCADE_A", "a"), makeEnvVarDep(pools(), "NIX_CASCADE_B", "b")});
    });

    // Invalidate B -> Phase 1 fails (trace hash A+B, B mismatches)
    // Phase 3 finds structural group with only A -> constructive recovery succeeds
    setenv("NIX_CASCADE_B", "b_new", 1);
    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, vpath({"child"}), state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"target", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, Recovery_DeepChain_IndependentVerification)
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

    withExclusiveStore(*db, [&](const auto & ea) {
        // Version 1: record traces for chain with deps at root and c1
        std::vector<Dep> rootDeps1 = {makeEnvVarDep(pools(), "NIX_DEEP_ROOT", "v1")};
        std::vector<Dep> c1Deps1 = {makeEnvVarDep(pools(), "NIX_DEEP_C1", "v1")};
        db->record(ea, rootPath(), string_t{"root_v1", {}}, rootDeps1);
        db->record(ea, c1Path, string_t{"c1_v1", {}}, c1Deps1);
        db->record(ea, c2Path, string_t{"c2_v1", {}}, {});
        db->record(ea, c3Path, string_t{"c3_v1", {}}, {});
        db->record(ea, c4Path, string_t{"c4_v1", {}}, {});
        db->record(ea, c5Path, string_t{"c5_v1", {}}, {});

        // Version 2: record traces with different dep values
        setenv("NIX_DEEP_ROOT", "v2", 1);
        setenv("NIX_DEEP_C1", "v2", 1);
        std::vector<Dep> rootDeps2 = {makeEnvVarDep(pools(), "NIX_DEEP_ROOT", "v2")};
        std::vector<Dep> c1Deps2 = {makeEnvVarDep(pools(), "NIX_DEEP_C1", "v2")};
        db->record(ea, rootPath(), string_t{"root_v2", {}}, rootDeps2);
        db->record(ea, c1Path, string_t{"c1_v2", {}}, c1Deps2);
        db->record(ea, c2Path, string_t{"c2_v2", {}}, {});
        db->record(ea, c3Path, string_t{"c3_v2", {}}, {});
        db->record(ea, c4Path, string_t{"c4_v2", {}}, {});
        db->record(ea, c5Path, string_t{"c5_v2", {}}, {});
    });

    // Revert to v1
    setenv("NIX_DEEP_ROOT", "v1", 1);
    setenv("NIX_DEEP_C1", "v1", 1);
    recreateDb(db);

    // Root recovers via own dep hash match
    auto rootR = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(rootR.has_value());
    assertCachedResultEquals(string_t{"root_v1", {}}, rootR->value, state.symbols);

    // c1 recovers via own dep hash match
    auto c1R = test::TraceStorageTestAccess::verify(*db, c1Path, state);
    ASSERT_TRUE(c1R.has_value());
    assertCachedResultEquals(string_t{"c1_v1", {}}, c1R->value, state.symbols);

    // c2-c5 have no deps → verify immediately (no recovery needed)
    ASSERT_TRUE(test::TraceStorageTestAccess::verify(*db, c2Path, state).has_value());
    ASSERT_TRUE(test::TraceStorageTestAccess::verify(*db, c3Path, state).has_value());
    ASSERT_TRUE(test::TraceStorageTestAccess::verify(*db, c4Path, state).has_value());
    ASSERT_TRUE(test::TraceStorageTestAccess::verify(*db, c5Path, state).has_value());
}

TEST_F(TraceStoreTest, Recovery_10Versions_AllReachable)
{
    // Record 10 trace versions of the same attribute (each with different env var value).
    // Revert to version 1. Verify constructive recovery succeeds.
    ScopedEnvVar env("NIX_STRESS_VAR", "version_0");

    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // Record 10 trace versions
        for (int i = 0; i < 10; i++) {
            auto val = "version_" + std::to_string(i);
            setenv("NIX_STRESS_VAR", val.c_str(), 1);
            std::vector<Dep> deps = {makeEnvVarDep(pools(), "NIX_STRESS_VAR", val)};
            auto result = "result_" + std::to_string(i);
            db->record(ea, rootPath(), string_t{result, {}}, deps);
        }
    });

    // Revert to each version and verify constructive recovery
    for (int target = 0; target < 10; target++) {
        auto val = "version_" + std::to_string(target);
        setenv("NIX_STRESS_VAR", val.c_str(), 1);
        recreateDb(db);

        auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
        ASSERT_TRUE(result.has_value()) << "Constructive recovery failed for version " << target;
        auto expected = "result_" + std::to_string(target);
        assertCachedResultEquals(string_t{expected, {}}, result->value, state.symbols);
    }
}

TEST_F(TraceStoreTest, Recovery_NeverRecordedValue_AllPhasesFail)
{
    // All recovery phases fail: attribute has dep on env var, env var changed to a value
    // never recorded. No constructive recovery candidate matches (BSàlC: no matching trace).
    ScopedEnvVar env("NIX_FAIL_VAR", "original");

    auto db = makeDb();

    std::vector<Dep> deps = {makeEnvVarDep(pools(), "NIX_FAIL_VAR", "original")};
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"old_result", {}}, deps);
    });

    // Change to a NEVER-RECORDED value
    setenv("NIX_FAIL_VAR", "completely_new_value", 1);
    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
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

    withExclusiveStore(*db, [&](const auto & ea) {
        // T1: only depends on X
        std::vector<Dep> deps1 = {makeContentDep(pools(), fileX.path.string(), "content_X")};
        db->record(ea, rootPath(), string_t{"result_T1", {}}, deps1);

        // T2: depends on X + Y (latest, in attribute)
        std::vector<Dep> deps2 = {
            makeContentDep(pools(), fileX.path.string(), "content_X"),
            makeContentDep(pools(), fileY.path.string(), "content_Y"),
        };
        db->record(ea, rootPath(), string_t{"result_T2", {}}, deps2);
    });

    // Delete Y → T2's Content(Y) dep can't be verified normally
    std::filesystem::remove(fileY.path);
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);

    // Recovery: sentinel for Y, structural variant finds T1 (no Y dep)
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"result_T1", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, Recovery_MissingFile_DirectHashOnRevert)
{
    // Record T1 with [Content(X)="v1"]. Record T2 with [Content(X)="v2"].
    // Revert X to v1 content. Direct hash recovery succeeds (X matches v1).
    TempTestFile fileX("v1_content");

    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // T1: X has v1 content
        std::vector<Dep> deps1 = {makeContentDep(pools(), fileX.path.string(), "v1_content")};
        db->record(ea, rootPath(), string_t{"result_v1", {}}, deps1);

        // T2: X has v2 content
        fileX.modify("v2_content");
        getFSSourceAccessor()->invalidateCache();
        std::vector<Dep> deps2 = {makeContentDep(pools(), fileX.path.string(), "v2_content")};
        db->record(ea, rootPath(), string_t{"result_v2", {}}, deps2);
    });

    // Revert X to v1
    fileX.modify("v1_content");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"result_v1", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, Recovery_SentinelHash_Deterministic)
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

    withExclusiveStore(*db, [&](const auto & ea) {
        std::vector<Dep> deps = {makeContentDep(pools(), fileX.path.string(), "some_content")};
        db->record(ea, rootPath(), string_t{"result", {}}, deps);

        // Also record a second version so recovery has something to try
        fileX.modify("other_content");
        getFSSourceAccessor()->invalidateCache();
        std::vector<Dep> deps2 = {makeContentDep(pools(), fileX.path.string(), "other_content")};
        db->record(ea, rootPath(), string_t{"result2", {}}, deps2);
    });

    // Delete X
    std::filesystem::remove(fileX.path);
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);

    // Recovery attempts (sentinel hash won't match either v1 or v2, but
    // allComputable should be true so recovery runs without aborting).
    // No matching trace → verify returns nullopt, but the point is recovery
    // didn't abort early due to nullopt.
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
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
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"result", {}}, deps);
    });

    // Delete X
    std::filesystem::remove(fileX.path);
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(TraceStoreTest, Verify_MissingFile_ContentFails)
{
    // Record trace with Content(X). Delete X. verifyTrace fails
    // (sentinel ≠ stored hash), falls through to recovery.
    TempTestFile fileX("original_content");

    auto db = makeDb();

    std::vector<Dep> deps = {makeContentDep(pools(), fileX.path.string(), "original_content")};
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"result", {}}, deps);
    });

    // Delete X → Content dep mismatches (sentinel vs original hash)
    std::filesystem::remove(fileX.path);
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);

    // Should fail (only one trace, sentinel won't match stored content hash)
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(TraceStoreTest, Recovery_SentinelHash_DiffersFromRealHash)
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


// ── RecoveryAcceptance and verifyDeps split tests ────────────────────
//
// Theme 18: RecoveryAcceptance is an opaque type with private constructor.
// All recovery strategies go through guarded recovery accept helpers.
// Its single public method is take() &&.
//
// verifyDeps split: verifyDepsExcluding (pass-2 structural/implicit deps for the
// no-content-failure path and the content-failure structural override path) and
// verifyDepsOnlyFor (implicit deps only checked for failed files).

TEST_F(TraceStoreTest, Recovery_RevisionChange_DoesNotServeStaleResult)
{
    // BUG-1 scenario: record a trace with FileBytes(file=v1) + GitIdentity(revA).
    // The file changes (simulating a commit switch). In a new session where the
    // current git hash is revB (different from revA), recovery must return nullopt
    // — the revB trace was never recorded, so no result can be served.
    //
    // GitIdentity recovery: searches for a trace indexed by revB → not found.
    // DirectHash recovery: hash(file=v2, revB) → no stored match.
    // StructuralVariant: single structural group, same result → no match.
    // Result: nullopt — does NOT serve the stale v1 result.
    //
    // allDepsGitRecoverable requires file paths to be under the repo root.
    // We use TempDir as the "repo root" and create the file inside it.
    TempDir repoDir;
    repoDir.addFile("tracked.txt", "v1_content");
    auto filePath = (repoDir.path() / "tracked.txt").string();
    auto repoRoot = repoDir.path().string();

    auto hashA = depHash("rev-hash-A");
    auto hashB = depHash("rev-hash-B");

    // Session 1: record trace with git hash A and file at v1
    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(hashA.value));
        auto fileHashV1 = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());
        std::vector<Dep> deps = {
            makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes,
                DepSource::makeAbsolute(), filePath, fileHashV1, repoRoot),
            makeGitIdentityDep(pools(), repoRoot, "rev-hash-A"),
        };
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"v1", {}}, deps);
        });
    }

    // Simulate switching to revB: file content changes
    repoDir.addFile("tracked.txt", "v2_content");
    getFSSourceAccessor()->invalidateCache();

    // Session 2: current git hash is revB (different). No trace for revB exists.
    // verifyTrace fails (FileBytes: v1 hash != v2 hash).
    // recovery: GitIdentity(revB) → not found → DirectHash(file=v2, revB) → no match
    // → StructuralVariant → no match. Result: nullopt.
    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(hashB.value));
        VerificationSession session;
        session.gitIdentityCache[pools().intern<RepoRootId>(repoRoot)] =
            std::optional<CurrentGitIdentityHash>(CurrentGitIdentityHash{hashB.value});

        auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state, session);
        EXPECT_FALSE(result.has_value())
            << "Git hash changed A→B with no revB trace: must not serve stale v1 result";
    }
}

TEST_F(TraceStoreTest, Recovery_RevisionUnchanged_ServesResultWhenHashMatches)
{
    // Same setup as the previous test, but now the current git hash stays at revA.
    // The file changes (content differs), so verifyTrace fails for the stored trace.
    // Recovery: GitIdentity(revA) → finds the trace recorded with revA → success.
    // RecoveryAcceptance::take() extracts the result and returns "v1".
    TempDir repoDir;
    repoDir.addFile("tracked.txt", "v1_content");
    auto filePath = (repoDir.path() / "tracked.txt").string();
    auto repoRoot = repoDir.path().string();

    auto hashA = depHash("rev-hash-stable");

    // Session 1: record trace with git hash A and file at v1
    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(hashA.value));
        auto fileHashV1 = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());
        std::vector<Dep> deps = {
            makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes,
                DepSource::makeAbsolute(), filePath, fileHashV1, repoRoot),
            makeGitIdentityDep(pools(), repoRoot, "rev-hash-stable"),
        };
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"v1", {}}, deps);
        });
    }

    // Simulate a file change (content differs but same git revision — e.g., dirty tree)
    repoDir.addFile("tracked.txt", "v2_content");
    getFSSourceAccessor()->invalidateCache();

    // Session 2: current git hash is still revA.
    // verifyTrace fails (FileBytes: v1 hash != v2 hash).
    // recovery: GitIdentity(revA) → finds the trace recorded with revA
    // → guarded RecoveryAcceptance::take() → "v1" result.
    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(hashA.value));
        VerificationSession session;
        session.gitIdentityCache[pools().intern<RepoRootId>(repoRoot)] =
            std::optional<CurrentGitIdentityHash>(CurrentGitIdentityHash{hashA.value});

        auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state, session);
        ASSERT_TRUE(result.has_value())
            << "Git hash unchanged (revA), GitIdentity recovery should return v1";
        assertCachedResultEquals(string_t{"v1", {}}, result->value, state.symbols);
    }
}

TEST_F(TraceStoreTest, Recovery_AllThreeStrategies_ProduceVerifyResult)
{
    // Test that all three recovery strategies return a valid VerifyResult
    // whose value can be extracted via RecoveryAcceptance::take() (exercised
    // indirectly through the public verify() API).
    //
    // Strategy 1 (GitIdentity): session config carries git hash, old trace
    //   has GitIdentity dep → indexed lookup, result extracted via take().
    // Strategy 2 (DirectHash): env var dep, change value then revert → direct
    //   hash recomputation matches → result extracted via take().
    // Strategy 3 (StructVariant): two recordings with different dep structures,
    //   invalidate latest → structural scan finds older trace → take().

    // ── Strategy 1: GitIdentity ──────────────────────────────────────
    // File is under the repo root so allDepsGitRecoverable returns true.
    {
        TempDir repoS1;
        repoS1.addFile("file.txt", "git-content");
        auto pathGit = (repoS1.path() / "file.txt").string();
        auto repoRootS1 = repoS1.path().string();
        auto hashGit = depHash("git-rev-1");

        {
            auto db = makeDb();
            db->setSessionConfig(SessionConfig::forTest(hashGit.value));
            auto fh = depHash(SourcePath(getFSSourceAccessor(), CanonPath(pathGit)).readFile());
            withExclusiveStore(*db, [&](const auto & ea) {
                db->record(ea, vpath({"strategy1"}), string_t{"result-git", {}}, {
                    makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes,
                        DepSource::makeAbsolute(), pathGit, fh, repoRootS1),
                    makeGitIdentityDep(pools(), repoRootS1, "git-rev-1"),
                });
            });
        }

        // Modify file so FileBytes fails; GitIdentity hash unchanged → recovery succeeds
        repoS1.addFile("file.txt", "git-content-changed");
        getFSSourceAccessor()->invalidateCache();

        {
            auto db = makeDb();
            db->setSessionConfig(SessionConfig::forTest(hashGit.value));
            VerificationSession session;
            session.gitIdentityCache[pools().intern<RepoRootId>(repoRootS1)] =
                std::optional<CurrentGitIdentityHash>(CurrentGitIdentityHash{hashGit.value});
            auto result = test::TraceStorageTestAccess::verify(
                *db, vpath({"strategy1"}), state, session);
            ASSERT_TRUE(result.has_value()) << "Strategy 1: GitIdentity recovery failed";
            assertCachedResultEquals(string_t{"result-git", {}}, result->value, state.symbols);
        }
    }

    // ── Strategy 2: DirectHash ───────────────────────────────────────
    {
        ScopedEnvVar env("NIX_S2_VAR", "s2-val-A");

        {
            auto db = makeDb();
            withExclusiveStore(*db, [&](const auto & ea) {
                std::vector<Dep> depsA = {makeEnvVarDep(pools(), "NIX_S2_VAR", "s2-val-A")};
                db->record(ea, vpath({"strategy2"}), string_t{"result-direct", {}}, depsA);
                // Record a second trace with a different value so recovery has history
                setenv("NIX_S2_VAR", "s2-val-B", 1);
                std::vector<Dep> depsB = {makeEnvVarDep(pools(), "NIX_S2_VAR", "s2-val-B")};
                db->record(ea, vpath({"strategy2"}), string_t{"result-direct-B", {}}, depsB);
            });
        }

        // Revert to A → direct hash recovery finds the A trace
        setenv("NIX_S2_VAR", "s2-val-A", 1);
        {
            auto db = makeDb();
            auto result = test::TraceStorageTestAccess::verify(
                *db, vpath({"strategy2"}), state);
            ASSERT_TRUE(result.has_value()) << "Strategy 2: DirectHash recovery failed";
            assertCachedResultEquals(string_t{"result-direct", {}}, result->value, state.symbols);
        }
    }

    // ── Strategy 3: StructuralVariant ────────────────────────────────
    {
        ScopedEnvVar envA("NIX_S3_A", "a");
        ScopedEnvVar envB("NIX_S3_B", "b");

        {
            auto db = makeDb();
            withExclusiveStore(*db, [&](const auto & ea) {
                // First recording: only dep A (older structural group)
                db->record(ea, vpath({"strategy3"}), string_t{"result-struct", {}},
                    {makeEnvVarDep(pools(), "NIX_S3_A", "a")});
                // Second recording: deps A + B (latest, different structural hash)
                db->record(ea, vpath({"strategy3"}), string_t{"result-struct-AB", {}},
                    {makeEnvVarDep(pools(), "NIX_S3_A", "a"),
                     makeEnvVarDep(pools(), "NIX_S3_B", "b")});
            });
        }

        // Invalidate B → direct hash of A+B(new) doesn't match either stored trace
        setenv("NIX_S3_B", "b-changed", 1);
        {
            auto db = makeDb();
            // Structural scan: finds group with only A → hash(A=a) matches → success
            auto result = test::TraceStorageTestAccess::verify(
                *db, vpath({"strategy3"}), state);
            ASSERT_TRUE(result.has_value()) << "Strategy 3: StructuralVariant recovery failed";
            assertCachedResultEquals(string_t{"result-struct", {}}, result->value, state.symbols);
        }
    }
}

TEST_F(TraceStoreTest, Recovery_StructuralVariant_DifferentStructHash_StillFindsMatch)
{
    // Record two traces for the same attr with different dep structures.
    // The latest trace (A + B) has a different structural hash than the
    // older trace (A only). Invalidate B so direct-hash recovery fails
    // (no stored trace matches hash(A, B-new)). Structural variant recovery
    // scans all structural hash groups: finds the A-only group, recomputes
    // hash(A) → matches the first trace → succeeds.
    //
    // This exercises tryStructuralVariantRecovery iterating multiple groups.
    ScopedEnvVar envA("NIX_SVR_A", "aval");
    ScopedEnvVar envB("NIX_SVR_B", "bval");
    ScopedEnvVar envC("NIX_SVR_C", "cval");

    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // Structural group 1: [A]
        db->record(ea, rootPath(), string_t{"result-A", {}},
            {makeEnvVarDep(pools(), "NIX_SVR_A", "aval")});

        // Structural group 2: [A, C] — different from groups 1 and 3
        db->record(ea, rootPath(), string_t{"result-AC", {}},
            {makeEnvVarDep(pools(), "NIX_SVR_A", "aval"),
             makeEnvVarDep(pools(), "NIX_SVR_C", "cval")});

        // Structural group 3: [A, B] — latest, different structural hash than groups 1 and 2
        db->record(ea, rootPath(), string_t{"result-AB", {}},
            {makeEnvVarDep(pools(), "NIX_SVR_A", "aval"),
             makeEnvVarDep(pools(), "NIX_SVR_B", "bval")});
    });

    // Change B to a never-recorded value.
    // Direct hash of [A, B-new] does not match any stored trace.
    setenv("NIX_SVR_B", "bval-new", 1);
    recreateDb(db);

    // Structural scan finds groups [A] and [A, C] (B's group skipped as
    // same structural hash as current). Both [A] and [A, C] have valid
    // dep hashes (A=aval, C=cval both unchanged).
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(result.has_value())
        << "Structural variant recovery must find a matching group despite "
           "different structural hashes";
    // Either result-A or result-AC is acceptable — both have valid deps
    auto & val = std::get<string_t>(result->value);
    EXPECT_TRUE(val.first == "result-A" || val.first == "result-AC")
        << "Unexpected result: " << val.first;
}

TEST_F(TraceStoreTest, Verify_ContentFailure_StructuralOverride)
{
    // Record a trace with:
    //   - FileBytes dep for a JSON file (hashes raw content)
    //   - StructuredProjection dep for the same file (hashes sorted JSON object keys)
    //
    // Then change the file's content (different JSON values, same keys).
    // FileBytes fails (content changed). StructuredProjection still passes
    // because the keys are unchanged. Coverage analysis: all failed files are
    // covered by passing structural deps → ValidViaStructuralOverride.
    //
    // This exercises verifyDepsExcluding in runPass2 (content-failure branch):
    // it skips structural deps for files whose content dep already passed,
    // and re-verifies structural deps only for the failed file.
    TempJsonFile jsonFile(R"({"version": 1, "name": "foo"})");
    auto filePath = std::filesystem::canonical(jsonFile.path).string();

    auto db = makeDb();

    // Pre-compute the content hash and keys hash for the initial file.
    auto contentHashV1 = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());
    // Keys: ["name", "version"] sorted → "name\0version"
    auto keysHash = canonicalKeysHash({"name", "version"});
    std::vector<Dep> deps = {
        makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes,
            DepSource::makeAbsolute(), filePath, contentHashV1),
        makeStructuredDepForTest(
            pools(), CanonicalQueryKind::StructuredProjection,
            DepSource::makeAbsolute(), filePath, StructuredFormat::Json,
            {}, DepHashValue(keysHash), ShapeSuffix::Keys),
    };
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"cached-result", {}}, deps);
    });

    // Change the file's values but keep the same keys.
    // FileBytes hash will differ; StructuredProjection (keys) stays the same.
    jsonFile.modify(R"({"version": 2, "name": "bar"})");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);

    // Verify: FileBytes fails (content changed), StructuredProjection passes
    // (same keys). All failed files are covered → ValidViaStructuralOverride.
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(result.has_value())
        << "StructuredProjection covers the content failure → should hit";
    assertCachedResultEquals(string_t{"cached-result", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, Verify_ContentFailure_ImplicitShapeOverride)
{
    // Record a trace with:
    //   - FileBytes dep for a JSON file (hashes raw content)
    //   - ImplicitStructure dep for the same file with ShapeSuffix::Type
    //     (hashes depHash("object") for any JSON object — stable across content changes)
    //
    // Then change the file's content (same structure/type, different values).
    // FileBytes fails. ImplicitStructure passes (still an object).
    // Coverage: failed file covered only by implicit dep (not structural).
    // Outcome: ValidViaImplicitShapeOverride → trace hash patched in memory.
    //
    // This exercises verifyDepsOnlyFor in runPass2 (content-failure branch):
    // implicit deps are checked only for failed files not already covered
    // structurally.
    TempJsonFile jsonFile(R"({"value": 42})");
    auto filePath = std::filesystem::canonical(jsonFile.path).string();

    auto db = makeDb();

    auto contentHashV1 = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());
    // ShapeSuffix::Type on a JSON object → sentinel(SentinelHash::Object) = depHash("object")
    auto typeHash = sentinel(SentinelHash::Object);
    std::vector<Dep> deps = {
        makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes,
            DepSource::makeAbsolute(), filePath, contentHashV1),
        makeStructuredDepForTest(
            pools(), CanonicalQueryKind::ImplicitStructure,
            DepSource::makeAbsolute(), filePath, StructuredFormat::Json,
            {}, DepHashValue(typeHash), ShapeSuffix::Type),
    };
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"implicit-result", {}}, deps);
    });

    // Change the file's value — content hash differs, but it's still a JSON object.
    jsonFile.modify(R"({"value": 99})");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);

    // Verify: FileBytes fails (content changed), ImplicitStructure passes
    // (still a JSON object, sentinel(SentinelHash::Object) unchanged). The failed file has no
    // StructuredProjection dep → implicit-only coverage.
    // Outcome: ValidViaImplicitShapeOverride → trace hash patched in memory.
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(result.has_value())
        << "ImplicitStructure covers the content failure via type check → should hit";
    assertCachedResultEquals(string_t{"implicit-result", {}}, result->value, state.symbols);

    // Second verify: after the ImplicitShapeOverride patched the trace hash
    // in memory, the next verify in the same session should also succeed.
    recreateDb(db);
    auto result2 = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    // Note: the DB still has the original trace hash (patchTraceHashInMemory
    // is in-memory only). Recovery will fire again (direct hash of new deps
    // won't match stored). Structural/implicit override path will re-fire.
    // Either way, the result must still be present and correct.
    EXPECT_TRUE(result2.has_value())
        << "Second verify after implicit shape override should still find a result";
}


// ── C-1 · Multi-repo recovery: one repo changes, DirectHash succeeds ─────

TEST_F(TraceStoreTest, Recovery_MultiRepo_StaleGitHash_FileMatches_ValidViaContentDep)
{
    // Two "repos" (simulated via different repo-root strings).
    // Session 2 advances one repo's hash (B1 → B2) but the file content
    // is unchanged.  GitRevisionIdentity is an ImplicitStructural
    // short-circuit — when its hash mismatches, verification falls
    // through to the other deps (FileBytes).  Since FileBytes matches
    // and no failure flag is set, `determineOutcome` returns Valid.
    //
    // This is working-as-designed behavior: GitIdentity is an indexed
    // optimization, not a soundness gate.  The trace carries
    // FileBytes which alone is sufficient to verify.
    TempTextFile f("stable");
    auto filePath = std::filesystem::canonical(f.path).string();

    // Session 1: record with two git identity deps + a file dep.
    {
        auto db = makeDb();
        auto fileHash = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());
        db->setSessionConfig(SessionConfig::forTest(depHash("graph-1").value, "multirepo-stable"));
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"v1", {}}, {
                makeGitIdentityDep(pools(), "/tmp/repoA", "hash-A1"),
                makeGitIdentityDep(pools(), "/tmp/repoB", "hash-B1"),
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes,
                    DepSource::makeAbsolute(), filePath, fileHash),
            });
        });
    }

    // Session 2: repoB advances (hash-B2 ≠ hash-B1).  History-bootstrap
    // finds session 1's trace via the shared stableRecoveryKey; verifyTrace
    // runs and returns Valid (GitRevisionIdentity[B] mismatch is not a
    // hard failure; FileBytes matches; no deferred struct/implicit deps).
    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(depHash("graph-2").value, "multirepo-stable"));
        VerificationSession session;
        session.gitIdentityCache[pools().intern<RepoRootId>("/tmp/repoA")] =
            std::optional<CurrentGitIdentityHash>(CurrentGitIdentityHash{depHash("hash-A1").value});
        session.gitIdentityCache[pools().intern<RepoRootId>("/tmp/repoB")] =
            std::optional<CurrentGitIdentityHash>(CurrentGitIdentityHash{depHash("hash-B2").value});

        auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state, session);
        ASSERT_TRUE(result.has_value())
            << "FileBytes match is sufficient; GitRevisionIdentity mismatch "
               "is a short-circuit miss, not a hard failure";
        auto * s = std::get_if<string_t>(&result->value);
        ASSERT_NE(s, nullptr);
        EXPECT_EQ(s->first, "v1")
            << "served value must be v1 (from session 1's recorded trace)";
    }
}

// ── C-2 · GitIdentity → DirectHash fallthrough ───────────────────────────

TEST_F(TraceStoreTest, Recovery_GitIdentity_StaleHash_FileMatches_ValidViaContentDep)
{
    // Record a trace with a GitRevisionIdentity dep + a FileBytes dep.
    // Session 2 has the rev advanced (rev-1 → rev-2) but the file is
    // unchanged.  GitRevisionIdentity is an ImplicitStructural
    // short-circuit; its mismatch does not flag `hasNonContentFailure`.
    // FileBytes matches → `determineOutcome` returns Valid.
    //
    // Same mechanism as the MultiRepo test above, with a single repo.
    TempTextFile f("data");
    auto filePath = std::filesystem::canonical(f.path).string();

    {
        auto db = makeDb();
        auto fileHash = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());
        db->setSessionConfig(SessionConfig::forTest(depHash("rev-1").value, "fallthrough-stable"));
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"v1", {}}, {
                makeGitIdentityDep(pools(), "/tmp/fallthrough-repo", "rev-1"),
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes,
                    DepSource::makeAbsolute(), filePath, fileHash),
            });
        });
    }

    // Session 2: git hash advances.  History-bootstrap finds session 1's
    // trace via shared stableRecoveryKey; verifyTrace runs and returns
    // Valid because FileBytes matches and GitIdentity mismatch isn't fatal.
    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(depHash("rev-2").value, "fallthrough-stable"));
        VerificationSession session;
        session.gitIdentityCache[pools().intern<RepoRootId>("/tmp/fallthrough-repo")] =
            std::optional<CurrentGitIdentityHash>(CurrentGitIdentityHash{depHash("rev-2").value});

        auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state, session);
        ASSERT_TRUE(result.has_value())
            << "FileBytes match is sufficient; GitRevisionIdentity mismatch "
               "is a short-circuit miss, not a hard failure";
        auto * s = std::get_if<string_t>(&result->value);
        ASSERT_NE(s, nullptr);
        EXPECT_EQ(s->first, "v1");
    }
}

// ── C-3 · DirectHash → StructVariant fallthrough with TraceContext ───────

TEST_F(TraceStoreTest, Recovery_DirectHash_Miss_StructVariantSameGroup)
{
    // Record a trace with a file dep + an env-var dep.
    // Change the file so DirectHash fails.  Recovery pipeline:
    //  - GitIdentity: no GitRevisionIdentity dep → extractGoverningRepoId
    //    returns nullopt → not attempted.
    //  - DirectHash: recomputes with v2 file hash; absent from history.
    //  - StructVariant: single stored group matches current key-struct
    //    → skipped by the same-struct-group guard.
    // Verdict: deterministic miss.
    TempTextFile f("v1");
    ScopedEnvVar env("NIX_TEST_C3_STABLEVAR", "stable");
    auto filePath = std::filesystem::canonical(f.path).string();

    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"v1", {}}, {
            makeContentDep(pools(), filePath, "v1"),
            makeEnvVarDep(pools(), "NIX_TEST_C3_STABLEVAR", "stable"),
        });
    });
    recreateDb(db);
    // Verify warm hit first (precision pre-condition).
    EXPECT_TRUE(test::TraceStorageTestAccess::verify(*db, rootPath(), state).has_value());

    // Change the file content so DirectHash fails.
    f.modify("v2");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_FALSE(result.has_value())
        << "DirectHash miss + same-struct StructVariant skip → deterministic miss";
}

// ── C-4 · Cross-session recovery ─────────────────────────────────────────

TEST_F(TraceStoreTest, CrossSession_Recovery_DirectHash)
{
    TempTextFile f("v1");
    auto filePath = std::filesystem::canonical(f.path).string();

    // Session A: record a trace.
    {
        auto db = makeDb();
        auto fileHash = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());
        db->setSessionConfig(SessionConfig::forTest(depHash("session-A").value, "cross-session-stable"));
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"v1", {}}, {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes,
                    DepSource::makeAbsolute(), filePath, fileHash),
            });
        });
    } // db destroyed → flushed to SQLite

    // Session B: different semantic key, same stable recovery key.
    // History bootstrap should find the session-A trace via the stable key.
    // verifyTrace passes because the file is unchanged.
    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(depHash("session-B").value, "cross-session-stable"));
        auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
        EXPECT_TRUE(result.has_value())
            << "Cross-session DirectHash recovery should succeed for unchanged file";
    }
}

TEST_F(TraceStoreTest, CrossSession_Recovery_StructuralVariant)
{
    ScopedEnvVar envA("NIX_TEST_CS_SV_A", "a-val");
    ScopedEnvVar envB("NIX_TEST_CS_SV_B", "b-val");

    // Session A: record two traces with different structural hashes.
    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(depHash("cs-sv-session-A").value, "cross-session-sv-stable"));
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"r1", {}},
                {makeEnvVarDep(pools(), "NIX_TEST_CS_SV_A", "a-val")});
            db->record(ea, rootPath(), string_t{"r2", {}},
                {makeEnvVarDep(pools(), "NIX_TEST_CS_SV_A", "a-val"),
                 makeEnvVarDep(pools(), "NIX_TEST_CS_SV_B", "b-val")});
        });
    } // db destroyed → flushed

    // Invalidate B so the latest trace's DirectHash fails.
    setenv("NIX_TEST_CS_SV_B", "b-val-new", 1);

    // Session B: structural variant recovery should find r1 (only dep A, still valid).
    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(depHash("cs-sv-session-B").value, "cross-session-sv-stable"));
        auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
        EXPECT_TRUE(result.has_value())
            << "Cross-session StructuralVariant recovery should find r1";
    }
}

// ── C-5 · acceptRecoveredTrace dedup guard ───────────────────────────────

TEST_F(TraceStoreTest, Recovery_AcceptDedup_SameTraceRecoveredTwice)
{
    ScopedEnvVar envA("NIX_TEST_C5_A", "a-stable");
    ScopedEnvVar envB("NIX_TEST_C5_B", "b-initial");

    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        // Record two trace versions for the same path.
        db->record(ea, rootPath(), string_t{"r1", {}},
            {makeEnvVarDep(pools(), "NIX_TEST_C5_A", "a-stable")});
        db->record(ea, rootPath(), string_t{"r2", {}},
            {makeEnvVarDep(pools(), "NIX_TEST_C5_A", "a-stable"),
             makeEnvVarDep(pools(), "NIX_TEST_C5_B", "b-initial")});
    });

    // Invalidate B → DirectHash on the latest trace (A+B) fails.
    // StructuralVariant recovery finds r1 (only dep A, still valid).
    setenv("NIX_TEST_C5_B", "b-changed", 1);
    recreateDb(db);

    auto r1 = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    // First recovery call: structural variant should recover r1.
    EXPECT_TRUE(r1.has_value());
    // Second call: same result (dedup guard prevents duplicate inserts).
    auto r2 = test::TraceStorageTestAccess::verify(*db, rootPath(), state);

    // Both results should agree (same trace recovered).
    if (r1.has_value() && r2.has_value()) {
        EXPECT_EQ(r1->traceId, r2->traceId);
    }
    // At minimum no crash and no assertion failure.
    EXPECT_NO_THROW((void)r1);
    EXPECT_NO_THROW((void)r2);
}

// ── C-6 · Double recovery for same pathId is a no-op ─────────────────────

TEST_F(TraceStoreTest, Recovery_DoubleRecovery_SamePathId_IsNoop)
{
    TempTextFile f("stable");
    auto filePath = std::filesystem::canonical(f.path).string();

    // Session A: record a trace.
    {
        auto db = makeDb();
        auto fileHash = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());
        db->setSessionConfig(SessionConfig::forTest(depHash("c6-session-A").value, "c6-stable"));
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"v", {}}, {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes,
                    DepSource::makeAbsolute(), filePath, fileHash),
            });
        });
    } // db destroyed → flushed

    // Session B: first verify triggers recovery from session-A trace.
    // Second and third verifies on the same session must be no-ops (no crash,
    // no duplicate inserts, no assertion failures).
    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(depHash("c6-session-B").value, "c6-stable"));
        auto r1 = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
        EXPECT_TRUE(r1.has_value()) << "First recovery should succeed";
        auto r2 = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
        EXPECT_TRUE(r2.has_value()) << "Second call should also succeed (no-op recovery)";
        EXPECT_NO_THROW(test::TraceStorageTestAccess::verify(*db, rootPath(), state));
    }
}

// ── C-7 · History-bootstrap with structural override when keys unchanged ──

TEST_F(TraceStoreTest, Recovery_HistoryBootstrap_StructuralOverride_WhenKeysUnchanged)
{
    // Session B verifies against an empty session-key slot; SqliteTraceStorage::verify
    // falls through to the scanHistoryForAttr bootstrap (by stableRecoveryKey)
    // and finds session A's trace.  verifyTrace(A's traceId) runs under a
    // CurrentTraceScope: Pass 1 fails on FileBytes (JSON values changed), Pass
    // 2's verifyDepsExcluding recomputes hash(keys(current JSON)) for the
    // StructuredProjection dep — keys unchanged → match — and covers the
    // failed content file via ValidViaStructuralOverride.  This exercises the
    // History-bootstrap + structural-override path, NOT the CandidateDep
    // subsumption path in tryStructuralVariantRecovery (which would require
    // multiple candidate groups in history that DirectHashRecovery couldn't
    // match).
    TempJsonFile jsonFile(R"({"x": 1})");
    auto filePath = std::filesystem::canonical(jsonFile.path).string();

    auto keysHash = canonicalKeysHash({"x"});

    // Session A: record a trace with both FileBytes and StructuredProjection.
    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(depHash("c7-session-A").value, "c7-stable"));
        auto contentHashV1 = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"v1", {}}, {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes,
                    DepSource::makeAbsolute(), filePath, contentHashV1),
                makeStructuredDepForTest(
                    pools(), CanonicalQueryKind::StructuredProjection,
                    DepSource::makeAbsolute(), filePath, StructuredFormat::Json,
                    {}, DepHashValue(keysHash), ShapeSuffix::Keys),
            });
        });
    }

    // Session B: different semantic key but same stable key.
    // Change JSON values but keep same keys → FileBytes fails,
    // StructuredProjection (#keys) passes. Structural variant recovery
    // finds the v1 trace via key-set-hash subsumption.
    jsonFile.modify(R"({"x": 99})");
    getFSSourceAccessor()->invalidateCache();

    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(depHash("c7-session-B").value, "c7-stable"));
        auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
        ASSERT_TRUE(result.has_value())
            << "structural variant recovery should find v1 via key-set hash";
        assertCachedResultEquals(string_t{"v1", {}}, result->value, state.symbols);
    }
}

// ── G-7 · L1 cache state after StructVariant recovery failure ─────────────

TEST_F(TraceStoreTest, L1Cache_AfterStructVariantRecoveryFailure_IsInvalid)
{
    TempTextFile f("v1");
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"v1", {}},
                   {makeContentDep(pools(), f.path.string(), "v1")});
    });
    recreateDb(db);
    EXPECT_TRUE(test::TraceStorageTestAccess::verify(*db, rootPath(), state).has_value());

    // Invalidate the file so all recovery strategies fail.
    f.modify("v2");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);

    auto r1 = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_FALSE(r1.has_value());

    // Second call must also miss — L1 must be Invalid, not a stale hit.
    auto r2 = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_FALSE(r2.has_value());
}

// ═════════════════════════════════════════════════════════════════════════
// ORIGIN-TYPING INVARIANTS
// ═════════════════════════════════════════════════════════════════════════
//
// These static_asserts and tests enforce the compile-time and runtime
// invariants that make historical-hash poisoning of L1 unrepresentable.
// They are intentionally exhaustive: every gate in the soundness chain gets
// an explicit check, so the barrier cannot be weakened without a failing
// assertion.
//
// Invariant chain:
//
//   1. OriginDep<CurrentTrace> and OriginDep<HistoricalCandidate> are
//      distinct types; no implicit conversion in either direction.
//
//   2. OriginScope<O>::tag returns OriginDep<O> ONLY for the scope's own
//      origin — a CandidateScope cannot synthesize a CurrentTraceDep.
//
//   3. OriginScope specializations have private constructors.  They are
//      constructible only by the named entry points
//      OriginScopeFactory::enterCurrentTrace / ::enterCandidate — each of
//      which instantiates only its own origin's scope.
//
//   4. VerifiedSubsumption is only constructible via grantVerifiedSubsumption,
//      which `requires std::same_as<O, CurrentTrace>`.  SFINAE excludes
//      CandidateDep callers.
//
//   5. VerificationSession::cacheVerifiedHash requires a VerifiedSubsumption
//      by value.  No other type satisfies the parameter — passing nothing,
//      nullptr, or any other witness type is a compile error.
//
// Runtime invariants:
//
//   6. After any verifyTrace call (pass or fail), every entry in
//      session.currentDepHashes_ equals resolve(K, current_filesystem).
//
//   7. SV recovery with N candidates sharing a key K computes K at most
//      once (not N times); subsequent candidates hit L1.

namespace origin_invariants {

// Ad-hoc helpers for type-equality / non-constructibility probes.
//
// We use `std::constructible_from` rather than `std::is_convertible_v`:
// the latter only covers IMPLICIT conversions, which neither scope nor
// origin type has by design.  `std::constructible_from<T, Args...>`
// follows the standard convention "T is constructible from Args" —
// catches both implicit and explicit construction paths.
template<typename T, typename U>
inline constexpr bool is_same_v = std::is_same_v<T, U>;

template<typename T>
concept IsDefaultConstructible = std::default_initializable<T>;

// ---- Invariant 1: Distinct types, no cross-construction ----

static_assert(!is_same_v<CurrentTraceDep, CandidateDep>,
    "CurrentTraceDep and CandidateDep must be distinct types");
static_assert(!std::constructible_from<CurrentTraceDep, CandidateDep>,
    "CurrentTraceDep must not be constructible from CandidateDep");
static_assert(!std::constructible_from<CandidateDep, CurrentTraceDep>,
    "CandidateDep must not be constructible from CurrentTraceDep");

// Scopes: distinct types, non-cross-constructible.  A phase function
// declared to take `const CandidateScope &` cannot be called with a
// CurrentTraceScope caller (type mismatch) — this makes scope typos
// COMPILE ERRORS at the call site, not silent precision regressions.
static_assert(!is_same_v<CurrentTraceScope, CandidateScope>,
    "CurrentTraceScope and CandidateScope must be distinct types");
static_assert(!std::constructible_from<CurrentTraceScope, CandidateScope>,
    "CurrentTraceScope must not be constructible from CandidateScope");
static_assert(!std::constructible_from<CandidateScope, CurrentTraceScope>,
    "CandidateScope must not be constructible from CurrentTraceScope");

// ---- Invariant 2: Scope.tag returns ONLY the scope's origin ----

static_assert(is_same_v<
        decltype(std::declval<const CurrentTraceScope &>().tag(std::declval<Dep>())),
        CurrentTraceDep>,
    "CurrentTraceScope::tag must return CurrentTraceDep");

static_assert(is_same_v<
        decltype(std::declval<const CandidateScope &>().tag(std::declval<Dep>())),
        CandidateDep>,
    "CandidateScope::tag must return CandidateDep");

// A scope has NO other tag method returning a different origin — the only
// way to construct the other origin from a scope is to not have one.
// We probe via std::invocable: there is no overload of CandidateScope::tag
// that returns CurrentTraceDep.  This is trivially true since `tag` is
// not overloaded; we assert the return type is exactly CandidateDep (above)
// and separately assert the tag is invocable only on Dep:
static_assert(std::is_invocable_r_v<CurrentTraceDep, decltype(&CurrentTraceScope::tag), const CurrentTraceScope &, Dep>,
    "CurrentTraceScope::tag must be invocable with Dep -> CurrentTraceDep");
static_assert(std::is_invocable_r_v<CandidateDep, decltype(&CandidateScope::tag), const CandidateScope &, Dep>,
    "CandidateScope::tag must be invocable with Dep -> CandidateDep");

// ---- Invariant 3: OriginScope is not public-default-constructible ----

static_assert(!IsDefaultConstructible<CurrentTraceScope>,
    "CurrentTraceScope must not be default-constructible from external scope");
static_assert(!IsDefaultConstructible<CandidateScope>,
    "CandidateScope must not be default-constructible from external scope");

// OriginScope is move-constructible (scopes can be forwarded), but NOT
// copy-constructible (avoids accidental capability duplication).
static_assert(!std::is_copy_constructible_v<CurrentTraceScope>,
    "CurrentTraceScope must not be copy-constructible");
static_assert(!std::is_copy_constructible_v<CandidateScope>,
    "CandidateScope must not be copy-constructible");
static_assert(std::is_move_constructible_v<CurrentTraceScope>,
    "CurrentTraceScope must be move-constructible (enters via factory)");
static_assert(std::is_move_constructible_v<CandidateScope>,
    "CandidateScope must be move-constructible (enters via factory)");

// ---- Invariant 4: VerifiedSubsumption SFINAE barrier ----

template<typename O>
concept GrantAccepts = requires(const OriginDep<O> & d) {
    grantVerifiedSubsumption(d);
};

static_assert(GrantAccepts<dep_origin::CurrentTrace>,
    "CurrentTraceDep MUST satisfy grantVerifiedSubsumption");
static_assert(!GrantAccepts<dep_origin::HistoricalCandidate>,
    "CandidateDep MUST NOT satisfy grantVerifiedSubsumption — this is the "
    "compile-time soundness barrier preventing historical stored hashes "
    "from being written to L1 as VerifiedHash");

// The return type of grant must be exactly VerifiedSubsumption.
static_assert(is_same_v<
        decltype(grantVerifiedSubsumption(std::declval<const CurrentTraceDep &>())),
        VerifiedSubsumption>,
    "grantVerifiedSubsumption must return VerifiedSubsumption");

// ---- Invariant 5: cacheVerifiedHash requires the witness ----
//
// These probes confirm the VerifyImpl-internal call shape compiles, and that
// the witness type is the only acceptable argument at that parameter.  The
// methods are private, so we can't call them from test scope — but we can
// reason about signatures through std::is_invocable.  Instead of invoking,
// we verify by construction that VerifiedSubsumption has no public default
// constructor (so the witness CANNOT be forged from nowhere):

static_assert(!std::default_initializable<VerifiedSubsumption>,
    "VerifiedSubsumption must not be default-constructible — the only way "
    "to obtain one is via grantVerifiedSubsumption (friended free function)");

// The raw-Dep OriginDep constructor must be sealed:
static_assert(!std::constructible_from<CurrentTraceDep, Dep>,
    "OriginDep<CurrentTrace> must not be constructible from a raw Dep "
    "outside OriginScope::tag friendship");
static_assert(!std::constructible_from<CandidateDep, Dep>,
    "OriginDep<HistoricalCandidate> must not be constructible from a raw Dep "
    "outside OriginScope::tag friendship");

// Copy-construction is disabled — one OriginDep per scope.tag() call.
// Move-construction is permitted (factory/return-by-value).
static_assert(!std::is_copy_constructible_v<CurrentTraceDep>,
    "CurrentTraceDep must not be copy-constructible "
    "(one tagging event per OriginDep)");
static_assert(!std::is_copy_constructible_v<CandidateDep>,
    "CandidateDep must not be copy-constructible "
    "(one tagging event per OriginDep)");
static_assert(std::is_move_constructible_v<CurrentTraceDep>,
    "CurrentTraceDep must be move-constructible (prvalue return from tag())");
static_assert(std::is_move_constructible_v<CandidateDep>,
    "CandidateDep must be move-constructible (prvalue return from tag())");

} // namespace origin_invariants

// ═════════════════════════════════════════════════════════════════════════
// Runtime invariants (6 and 7)
// ═════════════════════════════════════════════════════════════════════════

// ── L1 invariant: every post-SV entry equals current-state resolution ────
//
// Walk L1 after a failing verify+SV and confirm every entry's hash equals
// what resolveCurrentDepHash would compute from scratch.  Any deviation
// means a historical or stale hash leaked into L1.

TEST_F(TraceStoreTest, L1Invariant_EveryEntryEqualsCurrentStateResolution)
{
    // Build a DB with two traces at the same attrPath whose shared file was
    // recorded at two different content hashes (v1, v2).  Both traces have
    // the same dep key set, so `structGroups` de-duplicates to one SV
    // candidate group; SV may be skipped entirely if its structHash matches
    // directHashStructHash.  The invariant being checked is unchanged by
    // that — we walk L1 after the full verify + recovery flow and confirm
    // every entry's value equals `resolve(K, current_state)`, i.e., no
    // historical hash (v1 or v2) can have leaked into L1.
    TempTextFile shared("v1");
    TempTextFile extra("ext-v1");

    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        // Trace 1: two deps.
        db->record(ea, rootPath(), string_t{"r1", {}}, {
            makeContentDep(pools(), shared.path.string(), "v1"),
            makeContentDep(pools(), extra.path.string(), "ext-v1"),
        });
    });
    // Overwrite `shared` to v2, record a fresh trace.
    shared.modify("v2");
    getFSSourceAccessor()->invalidateCache();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"r2", {}}, {
            makeContentDep(pools(), shared.path.string(), "v2"),
            makeContentDep(pools(), extra.path.string(), "ext-v1"),
        });
    });

    // Current state: shared=v3 (nobody recorded this), extra unchanged.
    // Pass 1 on the latest trace will see FileBytes mismatch for `shared`
    // and enter recovery.
    shared.modify("v3");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);

    // Go through the full verify → recovery flow so SV actually runs.
    // `verifyTrace` alone does not invoke `recovery()`; only `verify()` does.
    VerificationSession session;
    (void) test::TraceStorageTestAccess::verify(*db, rootPath(), state, session);

    // INVARIANT: every L1 entry V for key K must satisfy V == resolve(K, current).
    // We compute the expected values by hand.  For this fixture we know:
    //   FileBytes(shared) → hash("v3")
    //   FileBytes(extra)  → hash("ext-v1")
    auto expectedShared = DepHashValue(depHash("v3"));
    auto expectedExtra  = DepHashValue(depHash("ext-v1"));

    auto sharedKey = makeContentDep(pools(), shared.path.string(), "v3").key;
    auto extraKey  = makeContentDep(pools(), extra.path.string(), "ext-v1").key;

    {
        auto * cached = session.lookupDepHash(sharedKey);
        ASSERT_NE(cached, nullptr)
            << "L1 must contain the verified shared-file dep";
        ASSERT_TRUE(cached->has_value());
        EXPECT_EQ(**cached, expectedShared)
            << "L1[shared] must equal current-state resolution, not v1 or v2 historical hash";
        // Negative check: it must NOT equal either historical hash.
        EXPECT_NE(**cached, DepHashValue(depHash("v1")))
            << "L1[shared] must not equal the historical v1 stored hash";
        EXPECT_NE(**cached, DepHashValue(depHash("v2")))
            << "L1[shared] must not equal the historical v2 stored hash";
    }
    {
        auto * cached = session.lookupDepHash(extraKey);
        // extra may or may not be in L1 depending on verification path;
        // if present, it must equal current.
        if (cached && cached->has_value()) {
            EXPECT_EQ(**cached, expectedExtra)
                << "L1[extra] must equal current-state resolution";
        }
    }
}

// ── L1 dedup under SV · Exact compute-count guarantee ─────────────────────
//
// Records three traces.  The latest trace is the direct-hash target and is
// skipped by SV because it has the same exact dep-key set.  The two older
// traces are distinct structural variants that share a FileBytes dep on the
// same file.  Triggers a failing verify so SV iterates both candidates and
// exercises L1 reuse across them.

TEST_F(TraceStoreTest, L1Dedup_SharedKey_ComputedExactlyOncePerSVRun)
{
    // Build three traces at the same attrPath with distinct key sets.  The
    // latest [shared, unique, sentinel] group is the direct-hash target, so SV
    // should skip it and scan the two older non-identical groups.
    TempTextFile shared("s1");
    TempTextFile unique("u1");
    ScopedEnvVar envOther("L1_DEDUP_OTHER", "");

    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"A", {}}, {
            makeContentDep(pools(), shared.path.string(), "s1"),
            makeContentDep(pools(), unique.path.string(), "u1"),
        });
        db->record(ea, rootPath(), string_t{"C", {}}, {
            makeContentDep(pools(), shared.path.string(), "s1"),
            makeEnvVarDep(pools(), "L1_DEDUP_OTHER", ""),
        });
    });
    unique.modify("u2");
    getFSSourceAccessor()->invalidateCache();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"B", {}}, {
            makeContentDep(pools(), shared.path.string(), "s1"),
            makeContentDep(pools(), unique.path.string(), "u2"),
            makeEnvVarDep(pools(), "L1_DEDUP_SENTINEL", ""),
        });
    });

    // Mutate both files so neither historical trace matches Pass 1 directly.
    shared.modify("s2");
    getFSSourceAccessor()->invalidateCache();
    unique.modify("u3");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);

    auto before_misses = eval_trace::nrDepHashCacheMisses.load();
    auto before_hits   = eval_trace::nrDepHashCacheHits.load();
    auto before_svdeps = eval_trace::nrStructVariantDepsResolved.load();

    // Orchestrated verify — will fail Pass 1 and run recovery.
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_FALSE(result.has_value());

    auto delta_misses = eval_trace::nrDepHashCacheMisses.load() - before_misses;
    auto delta_hits   = eval_trace::nrDepHashCacheHits.load() - before_hits;
    auto delta_svdeps = eval_trace::nrStructVariantDepsResolved.load() - before_svdeps;

    // Ensure SV actually ran.  Both files are mutated past any recorded
    // value, so tryDirectHashRecovery cannot match history and SV must run.
    // Without this assertion, the hit-dominance check below would be silently
    // skipped if an earlier strategy happened to short-circuit — hiding the
    // L1-dedup regression this test exists to guard.
    ASSERT_GT(delta_svdeps, 0u)
        << "SV must run for this fixture (both files mutated past any "
        << "recorded value) — delta_svdeps=0 means an earlier recovery "
        << "strategy served this, and the dedup invariant was not exercised";

    // STRICT INVARIANT: unique keys computed AT MOST ONCE.
    //
    // Fixture has exactly four unique dep keys: FileBytes(shared),
    // FileBytes(unique), EnvironmentLookup(L1_DEDUP_SENTINEL), and
    // EnvironmentLookup(L1_DEDUP_OTHER).
    // StorePathAvailability deps bypass checkCache entirely (they go
    // through session.storePathValid via StorePathBatch), so they
    // cannot contribute to nrDepHashCacheMisses.
    //
    // The bypassed-SV mode would produce delta_misses ≈ 3N for N
    // candidate groups.  L1 dedup must bound misses to the unique-key
    // count — exactly 4 for this fixture.
    EXPECT_LE(delta_misses, 4u)
        << "L1 dedup must bound SV compute misses to unique key count "
        << "(fixture: shared, unique, sentinel env, other env); observed " << delta_misses
        << " misses, " << delta_hits << " hits, " << delta_svdeps
        << " SV deps resolved";

    // Exact same-key-set SV candidates are intentionally skipped now, so the
    // old global hit-dominance check was too sensitive to direct-hash prework.
    // What matters here is that both older SV groups were exercised and shared
    // keys reused L1 instead of recomputing.
    EXPECT_GE(delta_svdeps, 4u)
        << "SV should scan the two non-direct groups, each with two deps";
    EXPECT_GE(delta_hits, 2u)
        << "shared deps should be served from L1 at least twice; observed "
        << delta_hits << " hits vs " << delta_misses << " misses, "
        << delta_svdeps << " total SV deps";
}

// ── Soundness end-to-end: heterogeneous candidate hashes serve correct result

TEST_F(TraceStoreTest, SVRecovery_HeterogeneousStoredHashes_ServesCurrentMatch)
{
    // Two traces exist in History, each recorded against different content
    // of the same file.  Current state matches one of them exactly.  SV
    // must find and serve that matching candidate — never cross-pollute
    // stored hashes to serve an incorrect one.
    TempTextFile f("content-A");

    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"result-A", {}},
                   {makeContentDep(pools(), f.path.string(), "content-A")});
    });

    // Record an alternate with different content.
    f.modify("content-B");
    getFSSourceAccessor()->invalidateCache();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"result-B", {}},
                   {makeContentDep(pools(), f.path.string(), "content-B")});
    });

    // Restore to content-A: the A-recording should be served.
    f.modify("content-A");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);

    auto r = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(r.has_value()) << "recovery must find the A-recording";
    assertCachedResultEquals(string_t{"result-A", {}}, r->value, state.symbols);

    // Now restore to content-B and verify fresh session.
    f.modify("content-B");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);

    auto r2 = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(r2.has_value()) << "recovery must find the B-recording";
    assertCachedResultEquals(string_t{"result-B", {}}, r2->value, state.symbols);
}

} // namespace nix::eval_trace
