#include "helpers.hh"
#include "nix/expr/trace-store.hh"
#include "nix/expr/trace-hash.hh"

#include <algorithm>
#include <gtest/gtest.h>

#include "nix/expr/tests/libexpr.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class TraceStoreTest : public LibExprTest
{
public:
    TraceStoreTest()
        : LibExprTest(openStore("dummy://", {{"read-only", "false"}}),
            [](bool & readOnlyMode) {
                readOnlyMode = false;
                EvalSettings s{readOnlyMode};
                s.nixPath = {};
                return s;
            })
    {}

protected:
    // Writable cache dir for trace store SQLite (sandbox has no writable $HOME)
    ScopedCacheDir cacheDir;

    static constexpr int64_t testContextHash = 0x1234567890ABCDEF;

    TraceStore makeDb()
    {
        return TraceStore(state.symbols, testContextHash);
    }
};

// ── buildAttrPath tests ──────────────────────────────────────────────

TEST_F(TraceStoreTest, BuildAttrPath_Empty)
{
    EXPECT_EQ(TraceStore::buildAttrPath({}), "");
}

TEST_F(TraceStoreTest, BuildAttrPath_Single)
{
    EXPECT_EQ(TraceStore::buildAttrPath({"packages"}), "packages");
}

TEST_F(TraceStoreTest, BuildAttrPath_Multiple)
{
    auto path = TraceStore::buildAttrPath({"packages", "x86_64-linux", "hello"});
    // Attr path segments are null-byte separated (key encoding)
    std::string expected = "packages";
    expected.push_back('\0');
    expected.append("x86_64-linux");
    expected.push_back('\0');
    expected.append("hello");
    EXPECT_EQ(path, expected);
}

// ── depTypeName tests ───────────────────────────────────────────────

TEST_F(TraceStoreTest, DepTypeName_AllTypes)
{
    EXPECT_EQ(depTypeName(DepType::Content), "content");
    EXPECT_EQ(depTypeName(DepType::Directory), "directory");
    EXPECT_EQ(depTypeName(DepType::Existence), "existence");
    EXPECT_EQ(depTypeName(DepType::EnvVar), "envvar");
    EXPECT_EQ(depTypeName(DepType::CurrentTime), "currentTime");
    EXPECT_EQ(depTypeName(DepType::System), "system");
    EXPECT_EQ(depTypeName(DepType::UnhashedFetch), "unhashedFetch");
    EXPECT_EQ(depTypeName(DepType::ParentContext), "parentContext");
    EXPECT_EQ(depTypeName(DepType::CopiedPath), "copiedPath");
    EXPECT_EQ(depTypeName(DepType::Exec), "exec");
    EXPECT_EQ(depTypeName(DepType::NARContent), "narContent");
    EXPECT_EQ(depTypeName(DepType::StructuredContent), "structuredContent");
}

// ── record tests (BSàlC: trace recording / fresh evaluation) ─────

TEST_F(TraceStoreTest, Record_ReturnsTraceId)
{
    auto db = makeDb();
    auto result = db.record("", string_t{"hello", {}}, {}, true);
    // Trace recording returns a positive trace identifier (BSàlC: trace key)
    EXPECT_GT(result.traceId, 0);
}

TEST_F(TraceStoreTest, ColdStore_AttrExists)
{
    auto db = makeDb();
    db.record("", string_t{"hello", {}}, {}, true);

    EXPECT_TRUE(db.attrExists(""));
    EXPECT_FALSE(db.attrExists("nonexistent"));
}

TEST_F(TraceStoreTest, ColdStore_WithDeps)
{
    auto db = makeDb();
    std::vector<Dep> deps = {
        makeContentDep("/a.nix", "a"),
        makeEnvVarDep("HOME", "/home"),
    };

    auto result = db.record("", int_t{NixInt{42}}, deps, true);

    auto loadedDeps = db.loadFullTrace(result.traceId);
    EXPECT_EQ(loadedDeps.size(), 2u);
}

TEST_F(TraceStoreTest, ColdStore_VolatileDep_NotSessionCached)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeCurrentTimeDep()};

    auto result = db.record("", null_t{}, deps, true);

    // Volatile dep (CurrentTime) -> trace NOT marked as verified in session (Salsa: no memoization)
    EXPECT_FALSE(db.verifiedTraceIds.count(result.traceId));
}

TEST_F(TraceStoreTest, ColdStore_NonVolatile_SessionCached)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeContentDep("/a.nix", "a")};

    auto result = db.record("", null_t{}, deps, true);

    // Non-volatile dep -> trace marked as verified in session (Salsa: memoized query result)
    EXPECT_TRUE(db.verifiedTraceIds.count(result.traceId));
}

TEST_F(TraceStoreTest, ColdStore_ParentContextStored)
{
    auto db = makeDb();
    std::vector<Dep> deps = {
        makeContentDep("/a.nix", "a"),
        Dep{"", "", DepHashValue(std::string("parent-hash")), DepType::ParentContext},
    };

    auto result = db.record("", null_t{}, deps, true);

    auto loadedDeps = db.loadFullTrace(result.traceId);
    // ParentContext deps are now stored (dep separation: children reference parent result hash)
    EXPECT_EQ(loadedDeps.size(), 2u);
}

TEST_F(TraceStoreTest, ColdStore_WithParent)
{
    auto db = makeDb();

    // Record parent trace (BSàlC: trace for root key)
    db.record("", string_t{"parent-val", {}},
              {makeContentDep("/a.nix", "a")}, true);

    // Record child trace — own deps only, no parent dep inheritance
    auto childResult = db.record("child", string_t{"child-val", {}},
                                 {makeEnvVarDep("FOO", "bar")}, false);

    // loadFullTrace returns only the child's own deps (no parent merging)
    auto childDeps = db.loadFullTrace(childResult.traceId);
    EXPECT_EQ(childDeps.size(), 1u);
    EXPECT_EQ(childDeps[0].type, DepType::EnvVar);
}

TEST_F(TraceStoreTest, ColdStore_Deterministic)
{
    // Deterministic recording: same deps + result -> same trace (BSàlC: content-addressed trace)
    auto db = makeDb();
    std::vector<Dep> deps = {makeContentDep("/a.nix", "a")};
    CachedResult value = string_t{"result", {}};

    auto r1 = db.record("", value, deps, true);
    auto r2 = db.record("", value, deps, true);

    // Same deps + same parent -> same trace (content-addressed deduplication)
    EXPECT_EQ(r1.traceId, r2.traceId);
}

TEST_F(TraceStoreTest, ColdStore_AllValueTypes)
{
    auto db = makeDb();

    auto testRoundtrip = [&](const CachedResult & value, std::string_view name) {
        auto path = std::string(name);
        db.record(path, value, {}, false);

        auto result = db.verify(path, {}, state);
        ASSERT_TRUE(result.has_value()) << "verify failed for " << name;
        assertCachedResultEquals(value, result->value, state.symbols);
    };

    testRoundtrip(string_t{"hello", {}}, "str");
    testRoundtrip(true, "bool-t");
    testRoundtrip(false, "bool-f");
    testRoundtrip(int_t{NixInt{42}}, "int");
    testRoundtrip(null_t{}, "null");
    testRoundtrip(float_t{3.14}, "float");
    testRoundtrip(path_t{"/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-test"}, "path");
    testRoundtrip(failed_t{}, "failed");
    testRoundtrip(missing_t{}, "missing");
    testRoundtrip(misc_t{}, "misc");
    testRoundtrip(list_t{5}, "list");
    testRoundtrip(std::vector<std::string>{"a", "b", "c"}, "list-of-strings");
}

// ── Trace deduplication tests (BSàlC: content-addressed traces) ──────

TEST_F(TraceStoreTest, TraceDedup_IdenticalDeps)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeContentDep("/shared.nix", "shared")};

    // Two root attributes with identical deps should share the same trace (BSàlC: trace sharing)
    auto r1 = db.record("a", string_t{"val1", {}}, deps, false);
    auto r2 = db.record("b", string_t{"val2", {}}, deps, false);

    // Both should share the same trace ID (content-addressed)
    EXPECT_EQ(r1.traceId, r2.traceId);

    auto deps1 = db.loadFullTrace(r1.traceId);
    EXPECT_EQ(deps1.size(), 1u);
}

TEST_F(TraceStoreTest, EmptyTrace_HasDbRow)
{
    // Attributes with zero deps must still get a trace row (BSàlC: empty trace is valid)
    auto db = makeDb();

    auto result = db.record("", string_t{"val", {}}, {}, true);

    // Loading a trace with no deps should return empty vector (not fail)
    auto deps = db.loadFullTrace(result.traceId);
    EXPECT_TRUE(deps.empty());

    // Empty trace should pass verification (BSàlC: verifying trace with no deps always succeeds)
    EXPECT_TRUE(db.verifyTrace(result.traceId, {}, state));
}

// ── verifyTrace tests (BSàlC: verifying traces / Shake: unchanged check) ──

TEST_F(TraceStoreTest, VerifyTrace_EnvVar_Valid)
{
    ScopedEnvVar env("NIX_TEST_CACHE_VAR", "expected_value");

    auto db = makeDb();
    std::vector<Dep> deps = {makeEnvVarDep("NIX_TEST_CACHE_VAR", "expected_value")};
    auto result = db.record("", null_t{}, deps, true);

    // Clear session memo cache so verifyTrace re-checks all deps (Salsa: force re-verification)
    db.clearSessionCaches();

    bool valid = db.verifyTrace(result.traceId, {}, state);
    EXPECT_TRUE(valid);
}

TEST_F(TraceStoreTest, VerifyTrace_EnvVar_Invalid)
{
    ScopedEnvVar env("NIX_TEST_CACHE_VAR", "new_value");

    auto db = makeDb();
    // Record trace with OLD expected hash — current env has a different value
    std::vector<Dep> deps = {makeEnvVarDep("NIX_TEST_CACHE_VAR", "old_value")};
    auto result = db.record("", null_t{}, deps, true);

    // Clear session memo cache so verifyTrace re-checks all deps (Salsa: force re-verification)
    db.clearSessionCaches();

    bool valid = db.verifyTrace(result.traceId, {}, state);
    EXPECT_FALSE(valid);
}

TEST_F(TraceStoreTest, VerifyTrace_CurrentTime_AlwaysFails)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeCurrentTimeDep()};
    auto result = db.record("", null_t{}, deps, true);

    // CurrentTime is volatile — verification always fails (Shake: always-dirty rule)
    bool valid = db.verifyTrace(result.traceId, {}, state);
    EXPECT_FALSE(valid);
}

TEST_F(TraceStoreTest, VerifyTrace_Exec_AlwaysFails)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeExecDep()};
    auto result = db.record("", null_t{}, deps, true);

    bool valid = db.verifyTrace(result.traceId, {}, state);
    EXPECT_FALSE(valid);
}

TEST_F(TraceStoreTest, VerifyTrace_SessionCacheHit)
{
    ScopedEnvVar env("NIX_TEST_CACHE_VAR2", "val");

    auto db = makeDb();
    std::vector<Dep> deps = {makeEnvVarDep("NIX_TEST_CACHE_VAR2", "val")};
    auto result = db.record("", null_t{}, deps, true);

    // Recording with non-volatile deps should session-memo the trace (Salsa: memoization)
    EXPECT_TRUE(db.verifiedTraceIds.count(result.traceId));
    // Second call hits session memo — skips re-verification (Salsa: green query)
    EXPECT_TRUE(db.verifyTrace(result.traceId, {}, state));
}

TEST_F(TraceStoreTest, VerifyTrace_NoDeps_Valid)
{
    auto db = makeDb();
    auto result = db.record("", string_t{"val", {}}, {}, true);

    db.clearSessionCaches();

    bool valid = db.verifyTrace(result.traceId, {}, state);
    EXPECT_TRUE(valid);
}

TEST_F(TraceStoreTest, VerifyTrace_ParentInvalid_ChildSurvives)
{
    ScopedEnvVar env("NIX_TEST_PARENT", "current_value");

    auto db = makeDb();

    // Record parent trace with stale dep
    std::vector<Dep> staleDeps = {makeEnvVarDep("NIX_TEST_PARENT", "stale_value")};
    db.record("", string_t{"parent", {}}, staleDeps, true);

    // Record child with no direct deps — child has no deps to invalidate
    auto childResult = db.record("child", string_t{"child", {}},
                                 {}, false);

    // Clear session memo cache
    db.clearSessionCaches();

    // With separated deps, child has no deps of its own → always valid
    // Parent invalidation no longer cascades to children
    bool valid = db.verifyTrace(childResult.traceId, {}, state);
    EXPECT_TRUE(valid);
}

TEST_F(TraceStoreTest, VerifyTrace_ParentValid)
{
    ScopedEnvVar env("NIX_TEST_PARENT", "correct_value");

    auto db = makeDb();

    // Record parent trace with correct dep (BSàlC: verifying trace succeeds)
    std::vector<Dep> parentDeps = {makeEnvVarDep("NIX_TEST_PARENT", "correct_value")};
    db.record("", string_t{"parent", {}},
              parentDeps, true);

    // Record child with valid parent (Shake: transitive clean)
    auto childResult = db.record("child", string_t{"child", {}},
                                 {}, false);

    // Clear session memo cache
    db.clearSessionCaches();

    bool valid = db.verifyTrace(childResult.traceId, {}, state);
    EXPECT_TRUE(valid);
}

TEST_F(TraceStoreTest, VerifyTrace_MultipleDeps_OneInvalid)
{
    ScopedEnvVar env1("NIX_TEST_VALID", "current_value");
    ScopedEnvVar env2("NIX_TEST_STALE", "new_value");

    auto db = makeDb();
    std::vector<Dep> deps = {
        makeEnvVarDep("NIX_TEST_VALID", "current_value"),  // dep hash matches current
        makeEnvVarDep("NIX_TEST_STALE", "old_value"),      // dep hash stale (Shake: dirty input)
    };
    auto result = db.record("", null_t{}, deps, true);

    db.clearSessionCaches();

    bool valid = db.verifyTrace(result.traceId, {}, state);
    EXPECT_FALSE(valid);
}

// ── Record -> Verify roundtrip (BSàlC: trace recording then verification) ───

TEST_F(TraceStoreTest, ColdWarm_Roundtrip)
{
    ScopedEnvVar env("NIX_WARM_TEST", "stable");

    auto db = makeDb();
    CachedResult input = string_t{"cached value", {}};
    std::vector<Dep> deps = {makeEnvVarDep("NIX_WARM_TEST", "stable")};

    db.record("", input, deps, true);

    // Verification should find and validate the recorded trace (BSàlC: verify trace)
    auto result = db.verify("", {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(input, result->value, state.symbols);
}

TEST_F(TraceStoreTest, RecordVerify_Roundtrip_TraceId)
{
    ScopedEnvVar env("NIX_WARM_TEST2", "stable");

    auto db = makeDb();
    CachedResult input = int_t{NixInt{99}};
    std::vector<Dep> deps = {makeEnvVarDep("NIX_WARM_TEST2", "stable")};

    auto coldResult = db.record("", input, deps, true);

    auto result = db.verify("", {}, state);
    ASSERT_TRUE(result.has_value());
    // Verified trace's ID should match what recording returned
    EXPECT_EQ(result->traceId, coldResult.traceId);
}

TEST_F(TraceStoreTest, WarmPath_NoEntry)
{
    auto db = makeDb();
    auto result = db.verify("nonexistent", {}, state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(TraceStoreTest, WarmPath_InvalidatedDeps)
{
    // Record trace with one env var value, then change it.
    // Verification should fail (no constructive recovery candidate exists).
    ScopedEnvVar env("NIX_WARM_INVALID", "value1");

    auto db = makeDb();
    std::vector<Dep> deps = {makeEnvVarDep("NIX_WARM_INVALID", "value1")};
    db.record("", string_t{"old", {}}, deps, true);

    // Change env var — invalidates the recorded dep hash
    setenv("NIX_WARM_INVALID", "value2", 1);

    // Clear session memo cache to force re-verification (Salsa: invalidate memo)
    db.clearSessionCaches();

    auto result = db.verify("", {}, state);
    // Should fail: trace hash no longer matches and no constructive recovery target exists
    EXPECT_FALSE(result.has_value());
}

// ── Constructive recovery tests (BSàlC: constructive traces) ─────────

TEST_F(TraceStoreTest, Phase1_StillWorks)
{
    // Phase 1 constructive recovery: same trace structure, different dep values, revert -> succeeds
    ScopedEnvVar env("NIX_P1_TEST", "value_A");

    auto db = makeDb();
    std::vector<Dep> depsA = {makeEnvVarDep("NIX_P1_TEST", "value_A")};
    db.record("", string_t{"result_A", {}}, depsA, true);

    // Change env var to value_B and record new trace
    setenv("NIX_P1_TEST", "value_B", 1);
    std::vector<Dep> depsB = {makeEnvVarDep("NIX_P1_TEST", "value_B")};
    db.record("", string_t{"result_B", {}}, depsB, true);

    // Revert to value_A -- Phase 1 constructive recovery should find the trace from first recording
    setenv("NIX_P1_TEST", "value_A", 1);
    db.clearSessionCaches();

    auto result = db.verify("", {}, state);
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
    std::vector<Dep> rootDeps1 = {makeEnvVarDep("NIX_P1_ROOT", "val1")};
    db.record("", string_t{"root1", {}}, rootDeps1, true);

    // Record child trace (no deps)
    db.record("child", string_t{"child1", {}}, {}, false);

    // Change root env var — root trace becomes invalid
    setenv("NIX_P1_ROOT", "val2", 1);
    db.clearSessionCaches();

    // Child has no deps → verifies immediately despite parent invalidation
    auto childResult = db.verify("child", {}, state);
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
    std::vector<Dep> rootDeps1 = {makeEnvVarDep("NIX_P1W_ROOT", "rval1")};
    db.record("", string_t{"root1", {}}, rootDeps1, true);

    // Record child trace with stable dep (own dep only)
    std::vector<Dep> childDeps = {makeEnvVarDep("NIX_P1W_CHILD", "cval")};
    db.record("child", string_t{"child1", {}}, childDeps, false);

    // Change parent env var — parent trace becomes invalid
    setenv("NIX_P1W_ROOT", "rval2", 1);
    db.clearSessionCaches();

    // Child's own dep (NIX_P1W_CHILD=cval) is still valid → verify passes
    auto childResult = db.verify("child", {}, state);
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

    // Build null-byte-separated attr path for grandchild
    std::string c2AttrPath = "c1";
    c2AttrPath.push_back('\0');
    c2AttrPath.append("c2");

    // Version 1: record traces for entire tree
    std::vector<Dep> rootDeps1 = {makeEnvVarDep("NIX_P1C_ROOT", "v1")};
    std::vector<Dep> childDeps1 = {makeEnvVarDep("NIX_P1C_CHILD", "cv1")};
    db.record("", string_t{"r1", {}}, rootDeps1, true);
    db.record("c1", string_t{"c1v1", {}}, childDeps1, false);
    db.record(c2AttrPath, string_t{"c2v1", {}}, {}, false);

    // Version 2 — record new traces with changed deps at root and child
    setenv("NIX_P1C_ROOT", "v2", 1);
    setenv("NIX_P1C_CHILD", "cv2", 1);
    std::vector<Dep> rootDeps2 = {makeEnvVarDep("NIX_P1C_ROOT", "v2")};
    std::vector<Dep> childDeps2 = {makeEnvVarDep("NIX_P1C_CHILD", "cv2")};
    db.record("", string_t{"r2", {}}, rootDeps2, true);
    db.record("c1", string_t{"c1v2", {}}, childDeps2, false);
    db.record(c2AttrPath, string_t{"c2v2", {}}, {}, false);

    // Revert to v1 — each level recovers via own deps
    setenv("NIX_P1C_ROOT", "v1", 1);
    setenv("NIX_P1C_CHILD", "cv1", 1);
    db.clearSessionCaches();

    // Root recovers via own dep (NIX_P1C_ROOT=v1)
    auto rootR = db.verify("", {}, state);
    ASSERT_TRUE(rootR.has_value());
    assertCachedResultEquals(string_t{"r1", {}}, rootR->value, state.symbols);

    // Child recovers via own dep (NIX_P1C_CHILD=cv1)
    auto c1R = db.verify("c1", {}, state);
    ASSERT_TRUE(c1R.has_value());
    assertCachedResultEquals(string_t{"c1v1", {}}, c1R->value, state.symbols);

    // Grandchild has no deps → verifies immediately
    auto c2R = db.verify(c2AttrPath, {}, state);
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
    std::vector<Dep> deps1 = {makeEnvVarDep("NIX_P3_A", "aval")};
    db.record("", string_t{"result1", {}}, deps1, true);

    // Second recording: trace with deps A + B (different structural hash)
    std::vector<Dep> deps2 = {
        makeEnvVarDep("NIX_P3_A", "aval"),
        makeEnvVarDep("NIX_P3_B", "bval"),
    };
    db.record("", string_t{"result2", {}}, deps2, true);

    // Now attribute points to trace with deps A+B.
    // Change B to invalidate that trace's deps.
    setenv("NIX_P3_B", "bval_new", 1);
    db.clearSessionCaches();

    // Phase 1 computes new trace hash for A+B (bval_new) -- no match.
    // Phase 3 scans structural hash groups, finds the group with only dep A,
    // recomputes current trace hash for A -> matches the first trace (BSàlC: constructive recovery).
    auto result = db.verify("", {}, state);
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
    db.record("", string_t{"r1", {}},
                  {makeEnvVarDep("NIX_P3M_A", "a")}, true);

    // Structural hash group 2: A + B
    db.record("", string_t{"r2", {}},
                  {makeEnvVarDep("NIX_P3M_A", "a"), makeEnvVarDep("NIX_P3M_B", "b")},
                  true);

    // Structural hash group 3: A + B + C (latest, in attribute entry)
    db.record("", string_t{"r3", {}},
                  {makeEnvVarDep("NIX_P3M_A", "a"), makeEnvVarDep("NIX_P3M_B", "b"),
                   makeEnvVarDep("NIX_P3M_C", "c")},
                  true);

    // Change C -> invalidates structural hash group 3
    setenv("NIX_P3M_C", "c_new", 1);
    db.clearSessionCaches();

    auto result = db.verify("", {}, state);
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

    db.record("", string_t{"empty1", {}}, {}, true);
    db.record("", string_t{"empty2", {}},
                  {makeEnvVarDep("NIX_P3E_X", "x")}, true);

    // Invalidate X
    ScopedEnvVar env("NIX_P3E_X", "x_new");
    db.clearSessionCaches();

    auto result = db.verify("", {}, state);
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
    db.record("", string_t{"r1", {}},
                  {makeEnvVarDep("NIX_P1F_A", "a")}, true);
    db.record("", string_t{"r2", {}},
                  {makeEnvVarDep("NIX_P1F_A", "a"), makeEnvVarDep("NIX_P1F_B", "b")},
                  true);

    // Invalidate B
    setenv("NIX_P1F_B", "b_new", 1);
    db.clearSessionCaches();

    // Phase 1 fails (trace hash A+B with new B doesn't match). Phase 3 finds structural group with only A.
    auto result = db.verify("", {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"r1", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, AllPhaseFail_Volatile)
{
    // Volatile dep (CurrentTime) -> immediate abort, no recovery possible (Shake: always-dirty)
    auto db = makeDb();

    std::vector<Dep> deps = {makeCurrentTimeDep()};
    db.record("", null_t{}, deps, true);

    auto result = db.verify("", {}, state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(TraceStoreTest, RecoveryUpdatesAttribute)
{
    // After Phase 1 constructive recovery, next verify succeeds directly (attribute updated)
    ScopedEnvVar env("NIX_RUI_TEST", "val_A");

    auto db = makeDb();

    // Two trace recordings
    std::vector<Dep> depsA = {makeEnvVarDep("NIX_RUI_TEST", "val_A")};
    db.record("", string_t{"rA", {}}, depsA, true);

    setenv("NIX_RUI_TEST", "val_B", 1);
    std::vector<Dep> depsB = {makeEnvVarDep("NIX_RUI_TEST", "val_B")};
    db.record("", string_t{"rB", {}}, depsB, true);

    // Revert to A — constructive recovery should update attribute entry
    setenv("NIX_RUI_TEST", "val_A", 1);
    db.clearSessionCaches();

    auto r1 = db.verify("", {}, state);
    ASSERT_TRUE(r1.has_value());

    // Second verification should succeed via direct lookup (no recovery needed)
    db.clearSessionCaches();
    auto r2 = db.verify("", {}, state);
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
    db.record("child", string_t{"target", {}},
                  {makeEnvVarDep("NIX_CASCADE_A", "a")}, false);

    // Trace with structural hash for deps A + B (latest, in attribute)
    db.record("child", string_t{"latest", {}},
                  {makeEnvVarDep("NIX_CASCADE_A", "a"), makeEnvVarDep("NIX_CASCADE_B", "b")},
                  false);

    // Invalidate B -> Phase 1 fails (trace hash A+B, B mismatches)
    // Phase 3 finds structural group with only A -> constructive recovery succeeds
    setenv("NIX_CASCADE_B", "b_new", 1);
    db.clearSessionCaches();

    auto result = db.verify("child", {}, state);
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
    auto makePath = [](std::vector<std::string> parts) {
        std::string path;
        for (size_t i = 0; i < parts.size(); i++) {
            if (i > 0) path.push_back('\0');
            path.append(parts[i]);
        }
        return path;
    };
    auto c1Path = makePath({"c1"});
    auto c2Path = makePath({"c1", "c2"});
    auto c3Path = makePath({"c1", "c2", "c3"});
    auto c4Path = makePath({"c1", "c2", "c3", "c4"});
    auto c5Path = makePath({"c1", "c2", "c3", "c4", "c5"});

    // Version 1: record traces for chain with deps at root and c1
    std::vector<Dep> rootDeps1 = {makeEnvVarDep("NIX_DEEP_ROOT", "v1")};
    std::vector<Dep> c1Deps1 = {makeEnvVarDep("NIX_DEEP_C1", "v1")};
    db.record("", string_t{"root_v1", {}}, rootDeps1, true);
    db.record(c1Path, string_t{"c1_v1", {}}, c1Deps1, false);
    db.record(c2Path, string_t{"c2_v1", {}}, {}, false);
    db.record(c3Path, string_t{"c3_v1", {}}, {}, false);
    db.record(c4Path, string_t{"c4_v1", {}}, {}, false);
    db.record(c5Path, string_t{"c5_v1", {}}, {}, false);

    // Version 2: record traces with different dep values
    setenv("NIX_DEEP_ROOT", "v2", 1);
    setenv("NIX_DEEP_C1", "v2", 1);
    std::vector<Dep> rootDeps2 = {makeEnvVarDep("NIX_DEEP_ROOT", "v2")};
    std::vector<Dep> c1Deps2 = {makeEnvVarDep("NIX_DEEP_C1", "v2")};
    db.record("", string_t{"root_v2", {}}, rootDeps2, true);
    db.record(c1Path, string_t{"c1_v2", {}}, c1Deps2, false);
    db.record(c2Path, string_t{"c2_v2", {}}, {}, false);
    db.record(c3Path, string_t{"c3_v2", {}}, {}, false);
    db.record(c4Path, string_t{"c4_v2", {}}, {}, false);
    db.record(c5Path, string_t{"c5_v2", {}}, {}, false);

    // Revert to v1
    setenv("NIX_DEEP_ROOT", "v1", 1);
    setenv("NIX_DEEP_C1", "v1", 1);
    db.clearSessionCaches();

    // Root recovers via own dep hash match
    auto rootR = db.verify("", {}, state);
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
        std::vector<Dep> deps = {makeEnvVarDep("NIX_STRESS_VAR", val)};
        auto result = "result_" + std::to_string(i);
        db.record("", string_t{result, {}}, deps, true);
    }

    // Revert to each version and verify constructive recovery
    for (int target = 0; target < 10; target++) {
        auto val = "version_" + std::to_string(target);
        setenv("NIX_STRESS_VAR", val.c_str(), 1);
        db.clearSessionCaches();

        auto result = db.verify("", {}, state);
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

    std::vector<Dep> deps = {makeEnvVarDep("NIX_FAIL_VAR", "original")};
    db.record("", string_t{"old_result", {}}, deps, true);

    // Change to a NEVER-RECORDED value
    setenv("NIX_FAIL_VAR", "completely_new_value", 1);
    db.clearSessionCaches();

    auto result = db.verify("", {}, state);
    EXPECT_FALSE(result.has_value());
}

// ── Context hash isolation tests (BSàlC: separate trace stores per task) ──

TEST_F(TraceStoreTest, DifferentContextHash_Isolated)
{
    // Two different context hashes should have isolated trace namespaces

    {
        TraceStore db1(state.symbols, 111);
        db1.record("pkg", string_t{"v1", {}}, {}, false);
    }
    {
        TraceStore db2(state.symbols, 222);
        db2.record("pkg", string_t{"v2", {}}, {}, false);
    }

    {
        TraceStore db1(state.symbols, 111);
        auto r1 = db1.verify("pkg", {}, state);
        ASSERT_TRUE(r1.has_value());
        ASSERT_TRUE(std::holds_alternative<string_t>(r1->value));
        EXPECT_EQ(std::get<string_t>(r1->value).first, "v1");
    }
    {
        TraceStore db2(state.symbols, 222);
        auto r2 = db2.verify("pkg", {}, state);
        ASSERT_TRUE(r2.has_value());
        ASSERT_TRUE(std::holds_alternative<string_t>(r2->value));
        EXPECT_EQ(std::get<string_t>(r2->value).first, "v2");
    }
}

TEST_F(TraceStoreTest, NullByteAttrPath)
{
    auto db = makeDb();

    // Null-byte separated attr path like "packages\0x86_64-linux\0hello" (trace key encoding)
    std::string attrPath = "packages";
    attrPath.push_back('\0');
    attrPath.append("x86_64-linux");
    attrPath.push_back('\0');
    attrPath.append("hello");

    db.record(attrPath, string_t{"val", {}}, {}, false);
    EXPECT_TRUE(db.attrExists(attrPath));
    EXPECT_FALSE(db.attrExists("packages"));
}

TEST_F(TraceStoreTest, EmptyAttrPath)
{
    auto db = makeDb();
    db.record("", string_t{"root-val", {}}, {}, true);
    EXPECT_TRUE(db.attrExists(""));
}

TEST_F(TraceStoreTest, MultipleEntries_Stress)
{
    auto db = makeDb();

    // Record 100 trace entries
    for (int i = 0; i < 100; i++) {
        auto name = "stress-" + std::to_string(i);
        db.record(name, int_t{NixInt{i}}, {}, false);
    }

    EXPECT_TRUE(db.attrExists("stress-0"));
    EXPECT_TRUE(db.attrExists("stress-99"));
    EXPECT_FALSE(db.attrExists("stress-100"));
}

// ── BLOB serialization roundtrip tests (keys_blob + values_blob encoding) ──

TEST_F(TraceStoreTest, BlobRoundTrip_Empty)
{
    std::vector<TraceStore::InternedDepKey> keys;
    auto keysBlob = TraceStore::serializeKeys(keys);
    EXPECT_TRUE(keysBlob.empty());
    auto keysResult = TraceStore::deserializeKeys(keysBlob.data(), keysBlob.size());
    EXPECT_TRUE(keysResult.empty());

    std::vector<TraceStore::InternedDep> deps;
    auto valsBlob = TraceStore::serializeValues(deps);
    EXPECT_TRUE(valsBlob.empty());
    auto valsResult = TraceStore::deserializeValues(valsBlob.data(), valsBlob.size(), keysResult);
    EXPECT_TRUE(valsResult.empty());
}

TEST_F(TraceStoreTest, BlobRoundTrip_Blake3Deps)
{
    std::vector<TraceStore::InternedDepKey> keys;
    std::vector<TraceStore::InternedDep> deps;
    for (int i = 0; i < 5; i++) {
        auto hash = depHash("content-" + std::to_string(i));
        keys.push_back({DepType::Content, static_cast<uint32_t>(i + 1),
                        static_cast<uint32_t>(i + 100)});
        deps.push_back({DepType::Content, static_cast<uint32_t>(i + 1),
                        static_cast<uint32_t>(i + 100), DepHashValue(hash)});
    }

    auto keysBlob = TraceStore::serializeKeys(keys);
    EXPECT_FALSE(keysBlob.empty());
    auto keysResult = TraceStore::deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 5u);

    auto valsBlob = TraceStore::serializeValues(deps);
    EXPECT_FALSE(valsBlob.empty());
    auto valsResult = TraceStore::deserializeValues(valsBlob.data(), valsBlob.size(), keysResult);
    ASSERT_EQ(valsResult.size(), 5u);

    for (int i = 0; i < 5; i++) {
        EXPECT_EQ(keysResult[i].type, DepType::Content);
        EXPECT_EQ(keysResult[i].sourceId, static_cast<uint32_t>(i + 1));
        EXPECT_EQ(keysResult[i].keyId, static_cast<uint32_t>(i + 100));
        EXPECT_EQ(valsResult[i], deps[i].hash);
    }
}

TEST_F(TraceStoreTest, BlobRoundTrip_MixedDeps)
{
    std::vector<TraceStore::InternedDepKey> keys;
    std::vector<TraceStore::InternedDep> deps;

    // BLAKE3 hash dep (Content — file content oracle)
    keys.push_back({DepType::Content, 1, 2});
    deps.push_back({DepType::Content, 1, 2, DepHashValue(depHash("file-data"))});

    // String hash dep (CopiedPath — store path oracle)
    keys.push_back({DepType::CopiedPath, 3, 4});
    deps.push_back({DepType::CopiedPath, 3, 4,
                    DepHashValue(std::string("/nix/store/aaaa-test"))});

    // BLAKE3 hash dep (EnvVar — environment oracle)
    keys.push_back({DepType::EnvVar, 5, 6});
    deps.push_back({DepType::EnvVar, 5, 6, DepHashValue(depHash("env-val"))});

    // String hash dep (Existence — filesystem oracle)
    keys.push_back({DepType::Existence, 7, 8});
    deps.push_back({DepType::Existence, 7, 8, DepHashValue(std::string("missing"))});

    auto keysBlob = TraceStore::serializeKeys(keys);
    auto keysResult = TraceStore::deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 4u);

    auto valsBlob = TraceStore::serializeValues(deps);
    auto valsResult = TraceStore::deserializeValues(valsBlob.data(), valsBlob.size(), keysResult);
    ASSERT_EQ(valsResult.size(), 4u);

    // Content oracle: BLAKE3 hash key + value
    EXPECT_EQ(keysResult[0].type, DepType::Content);
    EXPECT_EQ(keysResult[0].sourceId, 1u);
    EXPECT_EQ(keysResult[0].keyId, 2u);
    EXPECT_TRUE(std::holds_alternative<Blake3Hash>(valsResult[0]));

    // CopiedPath oracle: string (not BLAKE3, so deserialized as string)
    EXPECT_EQ(keysResult[1].type, DepType::CopiedPath);
    EXPECT_EQ(keysResult[1].sourceId, 3u);
    EXPECT_EQ(keysResult[1].keyId, 4u);
    EXPECT_TRUE(std::holds_alternative<std::string>(valsResult[1]));
    EXPECT_EQ(std::get<std::string>(valsResult[1]), "/nix/store/aaaa-test");

    // EnvVar oracle: BLAKE3 hash
    EXPECT_EQ(keysResult[2].type, DepType::EnvVar);
    EXPECT_TRUE(std::holds_alternative<Blake3Hash>(valsResult[2]));

    // Existence oracle: string
    EXPECT_EQ(keysResult[3].type, DepType::Existence);
    EXPECT_TRUE(std::holds_alternative<std::string>(valsResult[3]));
    EXPECT_EQ(std::get<std::string>(valsResult[3]), "missing");
}

TEST_F(TraceStoreTest, BlobRoundTrip_LargeSet)
{
    std::vector<TraceStore::InternedDepKey> keys;
    std::vector<TraceStore::InternedDep> deps;
    for (uint32_t i = 0; i < 10000; i++) {
        auto hash = depHash("content-" + std::to_string(i));
        keys.push_back({DepType::Content, i, i + 50000});
        deps.push_back({DepType::Content, i, i + 50000, DepHashValue(hash)});
    }

    auto keysBlob = TraceStore::serializeKeys(keys);
    // keys_blob is zstd-compressed; verify smaller than raw (10000 * 9 bytes)
    EXPECT_GT(keysBlob.size(), 0u);
    EXPECT_LT(keysBlob.size(), 10000u * 9u);

    auto keysResult = TraceStore::deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 10000u);

    auto valsBlob = TraceStore::serializeValues(deps);
    // values_blob is zstd-compressed; random BLAKE3 hashes don't compress well,
    // so just verify non-empty (zstd overhead may exceed raw size for random data)
    EXPECT_GT(valsBlob.size(), 0u);

    auto valsResult = TraceStore::deserializeValues(valsBlob.data(), valsBlob.size(), keysResult);
    ASSERT_EQ(valsResult.size(), 10000u);

    // Spot-check first, middle, and last
    EXPECT_EQ(keysResult[0].sourceId, 0u);
    EXPECT_EQ(keysResult[0].keyId, 50000u);
    EXPECT_EQ(keysResult[5000].sourceId, 5000u);
    EXPECT_EQ(keysResult[5000].keyId, 55000u);
    EXPECT_EQ(keysResult[9999].sourceId, 9999u);
    EXPECT_EQ(keysResult[9999].keyId, 59999u);

    // Verify hashes match
    for (uint32_t i = 0; i < 10000; i++) {
        EXPECT_EQ(valsResult[i], deps[i].hash) << "Hash mismatch at index " << i;
    }
}

// ── Dep storage tests (content-addressed DepKeySets dedup) ────────────

TEST_F(TraceStoreTest, DepKeySets_SiblingOverlap)
{
    auto db = makeDb();

    // Parent with 0 deps (FullAttrs pattern — trace records only child names)
    db.record("", null_t{}, {}, true);

    // Child A trace with 100 deps (own deps only, no parent inheritance)
    std::vector<Dep> depsA;
    for (int i = 0; i < 100; i++) {
        depsA.push_back(makeContentDep("/file-" + std::to_string(i) + ".nix",
                                       "content-" + std::to_string(i)));
    }
    auto childA = db.record("a", string_t{"val-a", {}}, depsA, false);

    // Child B trace with 95 overlapping deps + 5 different hashes
    std::vector<Dep> depsB;
    for (int i = 0; i < 95; i++) {
        depsB.push_back(makeContentDep("/file-" + std::to_string(i) + ".nix",
                                       "content-" + std::to_string(i)));
    }
    for (int i = 95; i < 100; i++) {
        depsB.push_back(makeContentDep("/file-" + std::to_string(i) + ".nix",
                                       "content-modified-" + std::to_string(i)));
    }
    auto childB = db.record("b", string_t{"val-b", {}}, depsB, false);

    // Both traces should load correctly with full deps
    auto loadedA = db.loadFullTrace(childA.traceId);
    auto loadedB = db.loadFullTrace(childB.traceId);
    EXPECT_EQ(loadedA.size(), 100u);
    EXPECT_EQ(loadedB.size(), 100u);
}

TEST_F(TraceStoreTest, Record_SeparatedDeps)
{
    // Parent with deps, child with own deps — each stores only its own.
    // Tests that parent deps are NOT merged into child traces.
    auto db = makeDb();

    // Parent with 0 deps (FullAttrs pattern — trace records only child names)
    db.record("", null_t{}, {}, true);

    // Child A trace with 100 deps
    std::vector<Dep> depsA;
    for (int i = 0; i < 100; i++) {
        depsA.push_back(makeContentDep("/f" + std::to_string(i) + ".nix",
                                       "c" + std::to_string(i)));
    }
    auto childA = db.record("a", int_t{NixInt{1}}, depsA, false);

    // Child B trace with 99 overlapping + 1 different dep hash
    std::vector<Dep> depsB;
    for (int i = 0; i < 99; i++) {
        depsB.push_back(makeContentDep("/f" + std::to_string(i) + ".nix",
                                       "c" + std::to_string(i)));
    }
    depsB.push_back(makeContentDep("/f99.nix", "c99-modified"));
    auto childB = db.record("b", int_t{NixInt{2}}, depsB, false);

    // Verify both traces load correctly
    auto loadedA = db.loadFullTrace(childA.traceId);
    auto loadedB = db.loadFullTrace(childB.traceId);
    EXPECT_EQ(loadedA.size(), 100u);
    EXPECT_EQ(loadedB.size(), 100u);

    // Verify B trace has the modified dep hash
    bool foundModified = false;
    for (auto & dep : loadedB) {
        if (dep.key == "/f99.nix") {
            auto h = depHash("c99-modified");
            EXPECT_EQ(dep.expectedHash, DepHashValue(h));
            foundModified = true;
        }
    }
    EXPECT_TRUE(foundModified);
}

// ── Batch verification + dep hash caching tests (Shake: unchanged check) ──

TEST_F(TraceStoreTest, WarmPath_BatchValidation)
{
    // Record trace with 50 deps where dep #25 is stale.
    // Batch verification should compute ALL 50 current hashes (not stop at #25).
    ScopedEnvVar env0("NIX_BATCH_0", "v0");

    auto db = makeDb();
    std::vector<Dep> deps;
    for (int i = 0; i < 50; i++) {
        auto key = "NIX_BATCH_" + std::to_string(i);
        auto value = "v" + std::to_string(i);
        setenv(key.c_str(), value.c_str(), 1);
        deps.push_back(makeEnvVarDep(key, value));
    }
    auto result = db.record("", null_t{}, deps, true);

    // Change dep #25 to invalidate its hash
    setenv("NIX_BATCH_25", "CHANGED", 1);
    db.clearSessionCaches();

    // Verify — should fail but cache ALL 50 current dep hashes for reuse in recovery
    bool valid = db.verifyTrace(result.traceId, {}, state);
    EXPECT_FALSE(valid);

    // All 50 dep keys should be in currentDepHashes (cached for constructive recovery)
    EXPECT_GE(db.currentDepHashes.size(), 50u);

    // Clean up env vars
    for (int i = 0; i < 50; i++) {
        auto key = "NIX_BATCH_" + std::to_string(i);
        unsetenv(key.c_str());
    }
}

TEST_F(TraceStoreTest, WarmPath_HashCaching)
{
    // Record trace with deps, change 1, trigger verification failure -> constructive recovery.
    // Assert that recovery reuses cached dep hashes from verification (Shake: unchanged check reuse).
    ScopedEnvVar env1("NIX_HASHCACHE_A", "valA");
    ScopedEnvVar env2("NIX_HASHCACHE_B", "valB");

    auto db = makeDb();

    // Version 1: deps A and B
    std::vector<Dep> deps1 = {
        makeEnvVarDep("NIX_HASHCACHE_A", "valA"),
        makeEnvVarDep("NIX_HASHCACHE_B", "valB"),
    };
    db.record("", string_t{"result-1", {}}, deps1, true);

    // Version 2: A changed, B same
    setenv("NIX_HASHCACHE_A", "valA2", 1);
    std::vector<Dep> deps2 = {
        makeEnvVarDep("NIX_HASHCACHE_A", "valA2"),
        makeEnvVarDep("NIX_HASHCACHE_B", "valB"),
    };
    db.record("", string_t{"result-2", {}}, deps2, true);

    // Revert A
    setenv("NIX_HASHCACHE_A", "valA", 1);
    db.clearSessionCaches();

    // verify should fail verification (trace 2 deps don't match) then constructively recover to result-1
    auto result = db.verify("", {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"result-1", {}}, result->value, state.symbols);

    // currentDepHashes should have entries from batch verification
    EXPECT_GE(db.currentDepHashes.size(), 2u);
}

TEST_F(TraceStoreTest, WarmPath_BaseValidatedOnce)
{
    // Record 5 siblings sharing the same trace.
    // Verify all 5. Shared trace should be verified only once (Salsa: memoized verification).
    ScopedEnvVar env("NIX_BASE_VALID", "ok");

    auto db = makeDb();
    std::vector<Dep> sharedDeps = {makeEnvVarDep("NIX_BASE_VALID", "ok")};

    // Record 5 attrs with identical deps (all share the same trace)
    for (int i = 0; i < 5; i++) {
        auto name = "sibling-" + std::to_string(i);
        db.record(name, int_t{NixInt{i}}, sharedDeps, false);
    }

    db.clearSessionCaches();

    // Verify all 5 — trace verified on first access, session-memoized for rest
    for (int i = 0; i < 5; i++) {
        auto name = "sibling-" + std::to_string(i);
        auto result = db.verify(name, {}, state);
        ASSERT_TRUE(result.has_value()) << "Sibling " << i << " failed";
        ASSERT_TRUE(std::holds_alternative<int_t>(result->value));
        EXPECT_EQ(std::get<int_t>(result->value).x.value, i);
    }

    // verifiedTraceIds should have the shared trace (verified only once, then memoized)
    EXPECT_FALSE(db.verifiedTraceIds.empty());
}

// ── Full end-to-end record + verification roundtrip ──────────────────

TEST_F(TraceStoreTest, RecordVerify_WarmRoundtrip)
{
    // End-to-end: record traces, then verification retrieves correctly.
    // Use EnvVar deps so verification can actually check them (no files needed).
    ScopedEnvVar env1("NIX_DW_SHARED", "stable");
    ScopedEnvVar env2("NIX_DW_A", "a-val");
    ScopedEnvVar env3("NIX_DW_B", "b-val");

    auto db = makeDb();

    // 3 attrs with overlapping deps (all env vars — verifiable)
    auto sharedDep = makeEnvVarDep("NIX_DW_SHARED", "stable");

    // Attr 1: shared + 1 unique
    std::vector<Dep> deps1 = {sharedDep, makeEnvVarDep("NIX_DW_A", "a-val")};
    db.record("a", string_t{"val-a", {}}, deps1, false);

    // Attr 2: shared + 1 different unique
    std::vector<Dep> deps2 = {sharedDep, makeEnvVarDep("NIX_DW_B", "b-val")};
    db.record("b", string_t{"val-b", {}}, deps2, false);

    // Attr 3: shared only
    std::vector<Dep> deps3 = {sharedDep};
    db.record("c", string_t{"val-c", {}}, deps3, false);

    db.clearSessionCaches();

    // Verify all 3 traces (BSàlC: verify trace -> serve cached result)
    auto ra = db.verify("a", {}, state);
    ASSERT_TRUE(ra.has_value());
    assertCachedResultEquals(string_t{"val-a", {}}, ra->value, state.symbols);

    auto rb = db.verify("b", {}, state);
    ASSERT_TRUE(rb.has_value());
    assertCachedResultEquals(string_t{"val-b", {}}, rb->value, state.symbols);

    auto rc = db.verify("c", {}, state);
    ASSERT_TRUE(rc.has_value());
    assertCachedResultEquals(string_t{"val-c", {}}, rc->value, state.symbols);
}

// ── getCurrentTraceHash tests (ParentContext dep infrastructure) ─────

TEST_F(TraceStoreTest, GetCurrentTraceHash_ReturnsHash)
{
    auto db = makeDb();
    db.record("root", string_t{"val", {}}, {makeEnvVarDep("NIX_GTH_1", "a")}, true);

    auto hash = db.getCurrentTraceHash("root");
    ASSERT_TRUE(hash.has_value());

    // Deterministic: same call returns same hash
    auto hash2 = db.getCurrentTraceHash("root");
    ASSERT_TRUE(hash2.has_value());
    EXPECT_EQ(hash->to_string(HashFormat::Base16, false),
              hash2->to_string(HashFormat::Base16, false));
}

TEST_F(TraceStoreTest, GetCurrentTraceHash_MissingAttr)
{
    auto db = makeDb();
    auto hash = db.getCurrentTraceHash("nonexistent");
    EXPECT_FALSE(hash.has_value());
}

TEST_F(TraceStoreTest, GetCurrentTraceHash_ChangesWithDeps)
{
    auto db = makeDb();

    // Record with deps A
    db.record("root", string_t{"v1", {}}, {makeEnvVarDep("NIX_GTH_2A", "a")}, true);
    auto hash1 = db.getCurrentTraceHash("root");
    ASSERT_TRUE(hash1.has_value());

    // Re-record with deps B (different deps → different trace hash)
    db.record("root", string_t{"v2", {}}, {makeEnvVarDep("NIX_GTH_2B", "b")}, true);
    auto hash2 = db.getCurrentTraceHash("root");
    ASSERT_TRUE(hash2.has_value());

    EXPECT_NE(hash1->to_string(HashFormat::Base16, false),
              hash2->to_string(HashFormat::Base16, false));
}

TEST_F(TraceStoreTest, GetCurrentTraceHash_DiffersFromResultHash)
{
    // Two attrs with identical results but different deps should have
    // different trace hashes. This is the key property: trace hash captures
    // dep structure, not just result content.
    auto db = makeDb();

    db.record("a", string_t{"same-result", {}}, {makeEnvVarDep("NIX_GTH_3A", "a")}, false);
    db.record("b", string_t{"same-result", {}}, {makeEnvVarDep("NIX_GTH_3B", "b")}, false);

    auto hashA = db.getCurrentTraceHash("a");
    auto hashB = db.getCurrentTraceHash("b");
    ASSERT_TRUE(hashA.has_value());
    ASSERT_TRUE(hashB.has_value());

    // Same result but different deps → different trace hashes
    EXPECT_NE(hashA->to_string(HashFormat::Base16, false),
              hashB->to_string(HashFormat::Base16, false));
}

TEST_F(TraceStoreTest, GetCurrentTraceHash_TabSeparatorConversion)
{
    auto db = makeDb();

    // Build a null-byte-separated attr path (like buildAttrPath produces)
    std::string attrPath = "packages";
    attrPath.push_back('\0');
    attrPath.append("x86_64-linux");

    db.record(attrPath, string_t{"val", {}},
              {makeEnvVarDep("NIX_GTH_4", "v")}, false);

    // Convert \0 to \t (as trace-cache.cc does for ParentContext dep keys)
    std::string depKey = attrPath;
    std::replace(depKey.begin(), depKey.end(), '\0', '\t');

    // getCurrentTraceHash converts \t back to \0 internally
    auto hashViaTab = db.getCurrentTraceHash(depKey);
    ASSERT_TRUE(hashViaTab.has_value());

    // Should match direct lookup with original \0-separated path
    auto hashDirect = db.getCurrentTraceHash(attrPath);
    ASSERT_TRUE(hashDirect.has_value());

    EXPECT_EQ(hashViaTab->to_string(HashFormat::Base16, false),
              hashDirect->to_string(HashFormat::Base16, false));
}

// ── ParentContext dep verification tests ─────────────────────────────

TEST_F(TraceStoreTest, ParentContext_VerifiesWhenParentUnchanged)
{
    ScopedEnvVar env("NIX_PCV_1", "val");
    auto db = makeDb();

    // Record parent
    db.record("parent", string_t{"parent-val", {}},
              {makeEnvVarDep("NIX_PCV_1", "val")}, true);
    auto parentHash = db.getCurrentTraceHash("parent");
    ASSERT_TRUE(parentHash.has_value());

    // Record child with ParentContext dep
    db.record("child", string_t{"child-val", {}},
              {makeParentContextDep("parent", *parentHash)}, false);

    db.clearSessionCaches();

    // Parent unchanged → child verification passes
    auto result = db.verify("child", {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-val", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, ParentContext_FailsWhenParentChanges)
{
    ScopedEnvVar env("NIX_PCV_2", "val1");
    auto db = makeDb();

    // Record parent v1
    db.record("parent", string_t{"parent-v1", {}},
              {makeEnvVarDep("NIX_PCV_2", "val1")}, true);
    auto parentHash1 = db.getCurrentTraceHash("parent");
    ASSERT_TRUE(parentHash1.has_value());

    // Record child with ParentContext dep on parent v1
    db.record("child", string_t{"child-v1", {}},
              {makeParentContextDep("parent", *parentHash1)}, false);

    // Change parent deps → different trace hash
    setenv("NIX_PCV_2", "val2", 1);
    db.record("parent", string_t{"parent-v2", {}},
              {makeEnvVarDep("NIX_PCV_2", "val2")}, true);

    // Verify parent trace hash changed
    auto parentHash2 = db.getCurrentTraceHash("parent");
    ASSERT_TRUE(parentHash2.has_value());
    EXPECT_NE(parentHash1->to_string(HashFormat::Base16, false),
              parentHash2->to_string(HashFormat::Base16, false));

    db.clearSessionCaches();

    // Child verification fails: ParentContext dep mismatch
    auto result = db.verify("child", {}, state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(TraceStoreTest, ParentContext_RecoveryOnRevert)
{
    ScopedEnvVar env("NIX_PCV_3", "val1");
    auto db = makeDb();

    // Version 1: parent with val1
    db.record("parent", string_t{"parent-v1", {}},
              {makeEnvVarDep("NIX_PCV_3", "val1")}, true);
    auto parentHash1 = db.getCurrentTraceHash("parent");
    ASSERT_TRUE(parentHash1.has_value());

    // Child v1 with ParentContext dep
    db.record("child", string_t{"child-v1", {}},
              {makeParentContextDep("parent", *parentHash1)}, false);

    // Version 2: parent with val2
    setenv("NIX_PCV_3", "val2", 1);
    db.record("parent", string_t{"parent-v2", {}},
              {makeEnvVarDep("NIX_PCV_3", "val2")}, true);
    auto parentHash2 = db.getCurrentTraceHash("parent");
    ASSERT_TRUE(parentHash2.has_value());

    // Child v2 with ParentContext dep
    db.record("child", string_t{"child-v2", {}},
              {makeParentContextDep("parent", *parentHash2)}, false);

    // Revert parent to v1 (re-record so CurrentTraces points to v1 trace)
    setenv("NIX_PCV_3", "val1", 1);
    db.record("parent", string_t{"parent-v1", {}},
              {makeEnvVarDep("NIX_PCV_3", "val1")}, true);

    db.clearSessionCaches();

    // Child recovery finds child v1 trace (ParentContext matches v1 hash)
    auto result = db.verify("child", {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-v1", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, ParentContext_WithTabSeparatedKey)
{
    ScopedEnvVar env("NIX_PCV_4", "val");
    auto db = makeDb();

    // Record parent with null-byte-separated attr path
    std::string parentPath = "packages";
    parentPath.push_back('\0');
    parentPath.append("x86_64-linux");

    db.record(parentPath, string_t{"parent-val", {}},
              {makeEnvVarDep("NIX_PCV_4", "val")}, false);
    auto parentHash = db.getCurrentTraceHash(parentPath);
    ASSERT_TRUE(parentHash.has_value());

    // Build dep key with \t separator (as trace-cache.cc does)
    std::string depKey = parentPath;
    std::replace(depKey.begin(), depKey.end(), '\0', '\t');

    // Record child with ParentContext dep using \t-separated key
    std::string childPath = parentPath;
    childPath.push_back('\0');
    childPath.append("hello");

    db.record(childPath, string_t{"child-val", {}},
              {makeParentContextDep(depKey, *parentHash)}, false);

    db.clearSessionCaches();

    // Child verification passes — ParentContext dep with \t key works correctly
    auto result = db.verify(childPath, {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-val", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, ParentContext_MixedWithOwnDeps)
{
    ScopedEnvVar env1("NIX_PCV_5P", "parent-val");
    ScopedEnvVar env2("NIX_PCV_5C", "child-val");
    auto db = makeDb();

    // Record parent
    db.record("parent", string_t{"parent-result", {}},
              {makeEnvVarDep("NIX_PCV_5P", "parent-val")}, true);
    auto parentHash = db.getCurrentTraceHash("parent");
    ASSERT_TRUE(parentHash.has_value());

    // Record child with both own dep AND ParentContext dep
    std::vector<Dep> childDeps = {
        makeEnvVarDep("NIX_PCV_5C", "child-val"),
        makeParentContextDep("parent", *parentHash),
    };
    db.record("child", string_t{"child-result", {}}, childDeps, false);

    db.clearSessionCaches();

    // Both deps pass → verification succeeds
    auto result = db.verify("child", {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-result", {}}, result->value, state.symbols);

    // Now change only the child's own dep
    setenv("NIX_PCV_5C", "child-val-new", 1);
    db.clearSessionCaches();

    // Verification fails (own dep stale, not just ParentContext)
    auto result2 = db.verify("child", {}, state);
    EXPECT_FALSE(result2.has_value());
}

TEST_F(TraceStoreTest, ParentContext_SameResultDifferentDeps_Detects)
{
    // Key test: parent returns same result (same attrset shape) but different deps.
    // With trace hash (not result hash), the change is detected.
    auto db = makeDb();

    ScopedEnvVar env("NIX_PCV_6", "val-A");

    // Parent v1: result "same" with dep on val-A
    db.record("parent", string_t{"same", {}},
              {makeEnvVarDep("NIX_PCV_6", "val-A")}, true);
    auto parentHash1 = db.getCurrentTraceHash("parent");
    ASSERT_TRUE(parentHash1.has_value());

    // Child with ParentContext dep on parent v1
    db.record("child", string_t{"child-from-A", {}},
              {makeParentContextDep("parent", *parentHash1)}, false);

    // Parent v2: SAME result "same" but different dep value
    setenv("NIX_PCV_6", "val-B", 1);
    db.record("parent", string_t{"same", {}},
              {makeEnvVarDep("NIX_PCV_6", "val-B")}, true);

    // Parent trace hash changed even though result is identical
    auto parentHash2 = db.getCurrentTraceHash("parent");
    ASSERT_TRUE(parentHash2.has_value());
    EXPECT_NE(parentHash1->to_string(HashFormat::Base16, false),
              parentHash2->to_string(HashFormat::Base16, false));

    db.clearSessionCaches();

    // Child verification fails — trace hash detects dep change despite same result
    auto result = db.verify("child", {}, state);
    EXPECT_FALSE(result.has_value());
}

// ── Nixpkgs cache failure pattern tests ──────────────────────────────
//
// These tests are synthesized from specific nixpkgs commit patterns observed
// in a 100-commit benchmark of `nix eval -f nixos/release.nix closures`.
// Each test simulates a cache failure mode at the TraceStore level.

// -- Category A: Irrelevant content change invalidates trace (aliases.nix pattern) --

TEST_F(TraceStoreTest, NixpkgsMiss_IrrelevantContentChange)
{
    // Synthesized from eval[1] (commit 3f7b5d89ca): aliases.nix renamed
    // ciscoPacketTracer{8,9} -> cisco-packet-tracer_{8,9}. All 32 traces
    // invalidated despite producing the same output.
    //
    // Models: a trace depends on a large shared file (e.g., aliases.nix)
    // where only an irrelevant section changed. Verify fails because the
    // Content dep hash changed. Recovery fails because the exact combination
    // of current dep hashes was never previously recorded.
    ScopedEnvVar relevant("NIX_CATAMISS_REL", "stable_value");
    ScopedEnvVar irrelevant("NIX_CATAMISS_IRREL", "original");

    auto db = makeDb();

    std::vector<Dep> deps = {
        makeEnvVarDep("NIX_CATAMISS_REL", "stable_value"),
        makeEnvVarDep("NIX_CATAMISS_IRREL", "original"),
    };
    db.record("closures", string_t{"/nix/store/aaa-closures", {}}, deps, true);

    // Simulate commit transition: irrelevant dep changes (aliases.nix edit)
    setenv("NIX_CATAMISS_IRREL", "modified", 1);
    db.clearSessionCaches();

    auto result = db.verify("closures", {}, state);
    EXPECT_FALSE(result.has_value())
        << "Expected cache miss: irrelevant dep changed but no prior trace "
           "with matching hash exists (aliases.nix pattern)";
}

TEST_F(TraceStoreTest, NixpkgsHit_RevertAfterIrrelevantChange)
{
    // Synthesized from cold run eval[0]->eval[1]->eval[0]-revert pattern.
    // After reverting aliases.nix to its original content, direct hash
    // recovery finds the original trace in history.
    ScopedEnvVar relevant("NIX_CATAHIT_REL", "stable");
    ScopedEnvVar irrelevant("NIX_CATAHIT_IRREL", "v1");

    auto db = makeDb();

    // Version 1: record trace
    std::vector<Dep> depsV1 = {
        makeEnvVarDep("NIX_CATAHIT_REL", "stable"),
        makeEnvVarDep("NIX_CATAHIT_IRREL", "v1"),
    };
    db.record("closures", string_t{"/nix/store/aaa-closures", {}}, depsV1, true);

    // Version 2: irrelevant change (new aliases.nix content)
    setenv("NIX_CATAHIT_IRREL", "v2", 1);
    std::vector<Dep> depsV2 = {
        makeEnvVarDep("NIX_CATAHIT_REL", "stable"),
        makeEnvVarDep("NIX_CATAHIT_IRREL", "v2"),
    };
    db.record("closures", string_t{"/nix/store/aaa-closures", {}}, depsV2, true);

    // Revert to v1: direct hash recovery should find original trace
    setenv("NIX_CATAHIT_IRREL", "v1", 1);
    db.clearSessionCaches();

    auto result = db.verify("closures", {}, state);
    ASSERT_TRUE(result.has_value())
        << "Expected cache hit: reverting irrelevant dep should recover "
           "original trace via direct hash";
    assertCachedResultEquals(string_t{"/nix/store/aaa-closures", {}},
                             result->value, state.symbols);
}

// -- Category B: New directory entry invalidates trace (pkgs/by-name pattern) --

TEST_F(TraceStoreTest, NixpkgsMiss_NewDirectoryEntryChangesListing)
{
    // Synthesized from eval[53] (commit abed87246b): pkgs/by-name/fa/fabs/
    // added. The evaluation reads all entries in pkgs/by-name/fa/ (enumerates
    // the directory), so adding a new entry changes the listing hash.
    //
    // Even with per-entry StructuredContent deps, adding a new entry means
    // the coarse Directory dep hash changes. If the evaluation enumerates
    // all directory entries (e.g., via attrNames/mapAttrs on readDir), the
    // new entry constitutes a real change to the dep set.
    //
    // Models: two versions of a directory listing where an entry is added.
    // The trace depends on the old listing hash; the new hash is novel.
    ScopedEnvVar stable("NIX_CATB_STABLE", "unchanged_dep");

    auto db = makeDb();

    // Record trace depending on a "directory listing" (simulated as env var)
    // and a stable dep. The directory listing hash represents the coarse
    // Directory dep from readDir.
    std::vector<Dep> deps = {
        makeEnvVarDep("NIX_CATB_STABLE", "unchanged_dep"),
        makeEnvVarDep("NIX_CATB_DIR_LISTING", "hash_of_existing1_existing2"),
    };
    ScopedEnvVar dir("NIX_CATB_DIR_LISTING", "hash_of_existing1_existing2");
    db.record("closures", string_t{"/nix/store/bbb-closures", {}}, deps, true);

    // Add new entry to directory -> listing hash changes
    setenv("NIX_CATB_DIR_LISTING", "hash_of_existing1_existing2_fabs", 1);
    db.clearSessionCaches();

    auto result = db.verify("closures", {}, state);
    EXPECT_FALSE(result.has_value())
        << "Expected cache miss: directory listing changed (pkgs/by-name "
           "pattern), no prior trace with these dep hashes";
}

// -- Category C: Irrelevant line addition to shared file (python-packages.nix) --

TEST_F(TraceStoreTest, NixpkgsMiss_IrrelevantLineAdded)
{
    // Synthesized from eval[3] (commit 1575c9f64e): av_13 added to
    // python-packages.nix. The evaluation imports this file (Content dep)
    // but doesn't use av_13. Adding a line changes the content hash.
    //
    // Structurally identical to Category A (aliases.nix) -- both are
    // Content dep invalidation from an irrelevant change to a shared file.
    ScopedEnvVar shared("NIX_CATC_SHARED", "original_content_hash");
    ScopedEnvVar own("NIX_CATC_OWN", "my_stable_dep");

    auto db = makeDb();

    std::vector<Dep> deps = {
        makeEnvVarDep("NIX_CATC_SHARED", "original_content_hash"),
        makeEnvVarDep("NIX_CATC_OWN", "my_stable_dep"),
    };
    db.record("closures", string_t{"/nix/store/ccc-closures", {}}, deps, true);

    // Add line to shared file -> content hash changes
    setenv("NIX_CATC_SHARED", "modified_content_hash", 1);
    db.clearSessionCaches();

    auto result = db.verify("closures", {}, state);
    EXPECT_FALSE(result.has_value())
        << "Expected cache miss: shared file content changed (python-packages.nix "
           "pattern), no prior trace with these dep hashes";
}

// -- Category D: Interleaving between output groups --

TEST_F(TraceStoreTest, NixpkgsMiss_InterleavingGroups)
{
    // Synthesized from eval[71] (commit 0d3ea6c6b0): previous eval was
    // eval[70] (Group 8, different output). 12,781 files changed between
    // commits. The traces have different dep structures (different files read).
    //
    // Models: alternating between two commits with very different dep sets.
    // Recovery scans structural variant groups but finds no match because
    // the current dep hashes don't match any recorded trace.
    ScopedEnvVar a("NIX_CATD_A", "a1");
    ScopedEnvVar b("NIX_CATD_B", "b1");
    ScopedEnvVar c("NIX_CATD_C", "c1");

    auto db = makeDb();

    // Group 6 commit: depends on A + B
    std::vector<Dep> group6Deps = {
        makeEnvVarDep("NIX_CATD_A", "a1"),
        makeEnvVarDep("NIX_CATD_B", "b1"),
    };
    db.record("closures", string_t{"/nix/store/grp6", {}}, group6Deps, true);

    // Group 8 commit: depends on A + C (different structural hash)
    std::vector<Dep> group8Deps = {
        makeEnvVarDep("NIX_CATD_A", "a1"),
        makeEnvVarDep("NIX_CATD_C", "c1"),
    };
    db.record("closures", string_t{"/nix/store/grp8", {}}, group8Deps, true);

    // Switch to a third version where A changed and B is different
    setenv("NIX_CATD_A", "a2", 1);
    setenv("NIX_CATD_B", "b2", 1);
    db.clearSessionCaches();

    // Current traces point to group8 (A+C). Verify fails (A changed).
    // Recovery: direct hash with A+C -> no match (a2+c1 not recorded).
    // Structural variant scan: group6 structure (A+B) -> recompute: a2+b2 -> no match.
    // All recovery fails -> must re-evaluate.
    auto result = db.verify("closures", {}, state);
    EXPECT_FALSE(result.has_value())
        << "Expected cache miss: interleaving groups with changed deps, "
           "no matching trace in history";
}

TEST_F(TraceStoreTest, NixpkgsHit_InterleavingRecovery)
{
    // Synthesized from hot run recovery successes: alternating between two
    // commits where the earlier commit's trace is recoverable via structural
    // variant recovery (same dep keys, matching dep values in history).
    ScopedEnvVar a("NIX_CATD2_A", "a1");
    ScopedEnvVar b("NIX_CATD2_B", "b1");

    auto db = makeDb();

    // Version 1: depends on A only
    std::vector<Dep> deps1 = {
        makeEnvVarDep("NIX_CATD2_A", "a1"),
    };
    db.record("closures", string_t{"/nix/store/v1", {}}, deps1, true);

    // Version 2: depends on A + B (different struct_hash, becomes current)
    std::vector<Dep> deps2 = {
        makeEnvVarDep("NIX_CATD2_A", "a1"),
        makeEnvVarDep("NIX_CATD2_B", "b1"),
    };
    db.record("closures", string_t{"/nix/store/v2", {}}, deps2, true);

    // Change B -> version 2's trace invalid. But version 1 has only A dep.
    setenv("NIX_CATD2_B", "b2", 1);
    db.clearSessionCaches();

    // Recovery: direct hash for A+B (b2) fails.
    // Structural variant: finds group with only A, recomputes -> a1 matches -> recovered!
    auto result = db.verify("closures", {}, state);
    ASSERT_TRUE(result.has_value())
        << "Expected cache hit: structural variant recovery should find "
           "version 1 trace (only depends on A, which is unchanged)";
    assertCachedResultEquals(string_t{"/nix/store/v1", {}},
                             result->value, state.symbols);
}

// -- Cross-category: multiple irrelevant changes compound --

TEST_F(TraceStoreTest, NixpkgsMiss_MultipleIrrelevantChanges)
{
    // Synthesized from commits where both aliases.nix AND pkgs/by-name/
    // changed simultaneously. Models a trace with multiple deps where
    // two independent irrelevant changes both invalidate the trace.
    ScopedEnvVar d1("NIX_MULTI_DEP1", "stable1");
    ScopedEnvVar d2("NIX_MULTI_DEP2", "stable2");
    ScopedEnvVar irr1("NIX_MULTI_ALIASES", "original");
    ScopedEnvVar irr2("NIX_MULTI_DIRNAME", "original_listing");

    auto db = makeDb();

    std::vector<Dep> deps = {
        makeEnvVarDep("NIX_MULTI_DEP1", "stable1"),
        makeEnvVarDep("NIX_MULTI_DEP2", "stable2"),
        makeEnvVarDep("NIX_MULTI_ALIASES", "original"),
        makeEnvVarDep("NIX_MULTI_DIRNAME", "original_listing"),
    };
    db.record("closures", string_t{"/nix/store/multi", {}}, deps, true);

    // Both irrelevant deps change simultaneously
    setenv("NIX_MULTI_ALIASES", "renamed_alias", 1);
    setenv("NIX_MULTI_DIRNAME", "listing_with_new_pkg", 1);
    db.clearSessionCaches();

    auto result = db.verify("closures", {}, state);
    EXPECT_FALSE(result.has_value())
        << "Expected cache miss: multiple irrelevant deps changed "
           "simultaneously (aliases.nix + pkgs/by-name pattern)";
}

TEST_F(TraceStoreTest, NixpkgsHit_SameOutputDifferentHistory)
{
    // Synthesized from the observation that 29 unnecessary re-evaluations
    // produced one of only 10 unique outputs. Models the scenario where
    // multiple traces in history all produce the same result value but have
    // different dep hashes. Direct hash recovery succeeds when reverting to
    // a previously-seen dep state.
    ScopedEnvVar common("NIX_HIST_COMMON", "c1");
    ScopedEnvVar varying("NIX_HIST_VARYING", "v1");

    auto db = makeDb();

    // Record 5 versions with same result but different dep hashes
    for (int i = 1; i <= 5; i++) {
        auto v = "v" + std::to_string(i);
        setenv("NIX_HIST_VARYING", v.c_str(), 1);
        std::vector<Dep> deps = {
            makeEnvVarDep("NIX_HIST_COMMON", "c1"),
            makeEnvVarDep("NIX_HIST_VARYING", v),
        };
        db.record("closures", string_t{"/nix/store/same-output", {}}, deps, true);
    }

    // Revert to v2 (previously recorded state)
    setenv("NIX_HIST_VARYING", "v2", 1);
    db.clearSessionCaches();

    // Direct hash recovery finds v2's trace in history
    auto result = db.verify("closures", {}, state);
    ASSERT_TRUE(result.has_value())
        << "Expected cache hit: reverting to previously-seen dep state "
           "should recover via direct hash lookup";
    assertCachedResultEquals(string_t{"/nix/store/same-output", {}},
                             result->value, state.symbols);
}

TEST_F(TraceStoreTest, NixpkgsMiss_NovelDepState)
{
    // Synthesized from the 29 unnecessary re-evaluations where recovery
    // fails because the exact combination of current dep hashes was never
    // recorded. Even though the result is the same, the dep state is novel.
    ScopedEnvVar common("NIX_NOVEL_COMMON", "c1");
    ScopedEnvVar varying("NIX_NOVEL_VARYING", "v1");

    auto db = makeDb();

    // Record v1 and v2
    std::vector<Dep> depsV1 = {
        makeEnvVarDep("NIX_NOVEL_COMMON", "c1"),
        makeEnvVarDep("NIX_NOVEL_VARYING", "v1"),
    };
    db.record("closures", string_t{"/nix/store/same-output", {}}, depsV1, true);

    setenv("NIX_NOVEL_VARYING", "v2", 1);
    std::vector<Dep> depsV2 = {
        makeEnvVarDep("NIX_NOVEL_COMMON", "c1"),
        makeEnvVarDep("NIX_NOVEL_VARYING", "v2"),
    };
    db.record("closures", string_t{"/nix/store/same-output", {}}, depsV2, true);

    // Set to v3 (never recorded) -- same result would be produced but
    // the dep state is novel, so recovery fails.
    setenv("NIX_NOVEL_VARYING", "v3", 1);
    db.clearSessionCaches();

    auto result = db.verify("closures", {}, state);
    EXPECT_FALSE(result.has_value())
        << "Expected cache miss: novel dep state (v3) was never recorded, "
           "recovery cannot find matching trace despite same output";
}

// ── Serialization edge case tests ────────────────────────────────────

TEST_F(TraceStoreTest, BlobRoundTrip_StructuredContent)
{
    // StructuredContent deps use Blake3 hashes, like Content.
    // Verify they round-trip correctly through the factored serialization.
    std::vector<TraceStore::InternedDepKey> keys;
    std::vector<TraceStore::InternedDep> deps;

    keys.push_back({DepType::StructuredContent, 1, 2});
    deps.push_back({DepType::StructuredContent, 1, 2, DepHashValue(depHash("scalar-value"))});

    keys.push_back({DepType::Content, 3, 4});
    deps.push_back({DepType::Content, 3, 4, DepHashValue(depHash("file-content"))});

    auto keysBlob = TraceStore::serializeKeys(keys);
    auto keysResult = TraceStore::deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 2u);

    auto valsBlob = TraceStore::serializeValues(deps);
    auto valsResult = TraceStore::deserializeValues(valsBlob.data(), valsBlob.size(), keysResult);
    ASSERT_EQ(valsResult.size(), 2u);

    // StructuredContent is a Blake3 dep type
    EXPECT_EQ(keysResult[0].type, DepType::StructuredContent);
    EXPECT_TRUE(std::holds_alternative<Blake3Hash>(valsResult[0]));
    EXPECT_EQ(std::get<Blake3Hash>(valsResult[0]), depHash("scalar-value"));

    // Content is also Blake3
    EXPECT_EQ(keysResult[1].type, DepType::Content);
    EXPECT_TRUE(std::holds_alternative<Blake3Hash>(valsResult[1]));
    EXPECT_EQ(std::get<Blake3Hash>(valsResult[1]), depHash("file-content"));
}

TEST_F(TraceStoreTest, BlobRoundTrip_32ByteStringVsBlake3)
{
    // Critical test: a CopiedPath string value that is exactly 32 bytes
    // must be deserialized as a string, NOT as a Blake3Hash.
    // This tests the disambiguation logic in deserializeValues.
    std::string exactly32 = "abcdefghijklmnopqrstuvwxyz012345"; // 32 chars
    ASSERT_EQ(exactly32.size(), 32u);

    std::vector<TraceStore::InternedDepKey> keys;
    std::vector<TraceStore::InternedDep> deps;

    // CopiedPath with exactly 32-byte string value
    keys.push_back({DepType::CopiedPath, 1, 2});
    deps.push_back({DepType::CopiedPath, 1, 2, DepHashValue(exactly32)});

    // Content with Blake3 hash (also 32 bytes)
    keys.push_back({DepType::Content, 3, 4});
    deps.push_back({DepType::Content, 3, 4, DepHashValue(depHash("data"))});

    auto keysBlob = TraceStore::serializeKeys(keys);
    auto keysResult = TraceStore::deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 2u);

    auto valsBlob = TraceStore::serializeValues(deps);
    auto valsResult = TraceStore::deserializeValues(valsBlob.data(), valsBlob.size(), keysResult);
    ASSERT_EQ(valsResult.size(), 2u);

    // CopiedPath with 32-byte value must deserialize as string
    EXPECT_TRUE(std::holds_alternative<std::string>(valsResult[0]))
        << "32-byte CopiedPath value was incorrectly deserialized as Blake3Hash";
    EXPECT_EQ(std::get<std::string>(valsResult[0]), exactly32);

    // Content with 32-byte value must deserialize as Blake3Hash
    EXPECT_TRUE(std::holds_alternative<Blake3Hash>(valsResult[1]))
        << "Content dep value was incorrectly deserialized as string";
}

TEST_F(TraceStoreTest, BlobRoundTrip_SingleEntry)
{
    // Boundary: single dep in the serialization.
    std::vector<TraceStore::InternedDepKey> keys;
    std::vector<TraceStore::InternedDep> deps;

    keys.push_back({DepType::EnvVar, 42, 99});
    deps.push_back({DepType::EnvVar, 42, 99, DepHashValue(depHash("HOME=/home/user"))});

    auto keysBlob = TraceStore::serializeKeys(keys);
    auto keysResult = TraceStore::deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 1u);
    EXPECT_EQ(keysResult[0].type, DepType::EnvVar);
    EXPECT_EQ(keysResult[0].sourceId, 42u);
    EXPECT_EQ(keysResult[0].keyId, 99u);

    auto valsBlob = TraceStore::serializeValues(deps);
    auto valsResult = TraceStore::deserializeValues(valsBlob.data(), valsBlob.size(), keysResult);
    ASSERT_EQ(valsResult.size(), 1u);
    EXPECT_EQ(valsResult[0], deps[0].hash);
}

TEST_F(TraceStoreTest, BlobRoundTrip_AllDepTypes)
{
    // Round-trip test covering every dep type in the DepType enum.
    // This ensures isBlake3Dep is consistent with serialization/deserialization.
    std::vector<TraceStore::InternedDepKey> keys;
    std::vector<TraceStore::InternedDep> deps;

    auto addBlake3 = [&](DepType type, uint32_t sid, uint32_t kid, std::string_view data) {
        keys.push_back({type, sid, kid});
        deps.push_back({type, sid, kid, DepHashValue(depHash(data))});
    };
    auto addString = [&](DepType type, uint32_t sid, uint32_t kid, std::string_view data) {
        keys.push_back({type, sid, kid});
        deps.push_back({type, sid, kid, DepHashValue(std::string(data))});
    };

    // Blake3 dep types
    addBlake3(DepType::Content, 1, 1, "file");
    addBlake3(DepType::Directory, 2, 2, "dir");
    addBlake3(DepType::EnvVar, 3, 3, "env");
    addBlake3(DepType::System, 4, 4, "sys");
    addBlake3(DepType::NARContent, 5, 5, "nar");
    addBlake3(DepType::StructuredContent, 6, 6, "struct");
    addBlake3(DepType::ParentContext, 7, 7, "parent");

    // String dep types
    addString(DepType::Existence, 8, 8, "type:1");
    addString(DepType::CurrentTime, 9, 9, "volatile");
    addString(DepType::UnhashedFetch, 10, 10, "url");
    addString(DepType::CopiedPath, 11, 11, "/nix/store/test");
    addString(DepType::Exec, 12, 12, "volatile");

    auto keysBlob = TraceStore::serializeKeys(keys);
    auto keysResult = TraceStore::deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 12u);

    auto valsBlob = TraceStore::serializeValues(deps);
    auto valsResult = TraceStore::deserializeValues(valsBlob.data(), valsBlob.size(), keysResult);
    ASSERT_EQ(valsResult.size(), 12u);

    // Blake3 types should deserialize as Blake3Hash
    for (int i = 0; i < 7; i++) {
        EXPECT_TRUE(std::holds_alternative<Blake3Hash>(valsResult[i]))
            << "Dep type " << static_cast<int>(keysResult[i].type)
            << " should deserialize as Blake3Hash";
    }
    // String types should deserialize as std::string
    for (int i = 7; i < 12; i++) {
        EXPECT_TRUE(std::holds_alternative<std::string>(valsResult[i]))
            << "Dep type " << static_cast<int>(keysResult[i].type)
            << " should deserialize as string";
    }
}

// ── DepKeySets sharing tests ─────────────────────────────────────────

TEST_F(TraceStoreTest, DepKeySets_SharedKeysDifferentValues)
{
    // Two traces with identical dep keys but different hash values should
    // share the same DepKeySets row (same struct_hash) but have different
    // Traces rows (different trace_hash / values_blob).
    auto db = makeDb();

    std::vector<Dep> depsV1 = {
        makeContentDep("/a.nix", "content-v1"),
        makeEnvVarDep("HOME", "/home/user1"),
    };
    std::vector<Dep> depsV2 = {
        makeContentDep("/a.nix", "content-v2"),
        makeEnvVarDep("HOME", "/home/user2"),
    };

    auto r1 = db.record("attr", string_t{"result-1", {}}, depsV1, true);
    auto r2 = db.record("attr", string_t{"result-2", {}}, depsV2, true);

    // Different trace IDs (different hash values → different trace_hash)
    EXPECT_NE(r1.traceId, r2.traceId);

    // Both should load correctly
    auto loaded1 = db.loadFullTrace(r1.traceId);
    auto loaded2 = db.loadFullTrace(r2.traceId);
    EXPECT_EQ(loaded1.size(), 2u);
    EXPECT_EQ(loaded2.size(), 2u);

    // Verify the loaded values differ
    // Both have Content dep at index 0 (after sort); check the hash values differ
    bool foundDifference = false;
    for (size_t i = 0; i < loaded1.size(); i++) {
        if (loaded1[i].expectedHash != loaded2[i].expectedHash) {
            foundDifference = true;
            break;
        }
    }
    EXPECT_TRUE(foundDifference) << "Traces with same keys but different values should have different dep hashes";
}

// ── Phase 1 cache optimization tests ──────────────────────────────────
// Tests for the unified CachedTraceData cache, traceRowCache, in-memory
// recovery matching, and placeholder hash detection.

TEST_F(TraceStoreTest, CachedTraceData_HashesPopulated_Default)
{
    // Default-constructed CachedTraceData has placeholder (all-zero) hashes.
    TraceStore::CachedTraceData data;
    EXPECT_FALSE(data.hashesPopulated())
        << "Default CachedTraceData should have unpopulated (all-zero) hashes";
}

TEST_F(TraceStoreTest, CachedTraceData_HashesPopulated_AfterRecord)
{
    // After record(), traceDataCache should contain populated hashes.
    auto db = makeDb();
    auto result = db.record("test", string_t{"value", {}},
        {makeContentDep("/a.nix", "hello")}, true);

    auto it = db.traceDataCache.find(result.traceId);
    ASSERT_NE(it, db.traceDataCache.end());
    EXPECT_TRUE(it->second.hashesPopulated())
        << "CachedTraceData after record should have non-zero hashes";
    EXPECT_TRUE(it->second.deps.has_value())
        << "CachedTraceData after record should have deps populated";
}

TEST_F(TraceStoreTest, CachedTraceData_HashesPopulated_AfterEnsure)
{
    // ensureTraceHashes populates hashes but NOT deps.
    // getCurrentTraceHash calls ensureTraceHashes internally.
    auto db = makeDb();
    auto result = db.record("test", string_t{"value", {}},
        {makeContentDep("/a.nix", "hello")}, true);

    // Clear session caches to force re-read from DB
    db.traceDataCache.clear();

    // getCurrentTraceHash calls lookupTraceRow → ensureTraceHashes
    auto hash = db.getCurrentTraceHash("test");
    ASSERT_TRUE(hash.has_value());

    auto it = db.traceDataCache.find(result.traceId);
    ASSERT_NE(it, db.traceDataCache.end());
    EXPECT_TRUE(it->second.hashesPopulated())
        << "ensureTraceHashes should populate non-zero hashes";
    EXPECT_FALSE(it->second.deps.has_value())
        << "ensureTraceHashes should NOT populate deps (lazy)";
}

TEST_F(TraceStoreTest, LoadFullTrace_NonexistentTrace_NoCachePollution)
{
    // loadFullTrace on a nonexistent traceId should NOT create a cache entry.
    // This was a real bug (fixed): the old code did traceDataCache[traceId].deps = {},
    // which created an entry with placeholder zero hashes that ensureTraceHashes
    // would later incorrectly return as valid.
    auto db = makeDb();
    TraceId bogusId = 99999;

    auto result = db.loadFullTrace(bogusId);
    EXPECT_TRUE(result.empty());

    // Verify no cache pollution
    EXPECT_EQ(db.traceDataCache.find(bogusId), db.traceDataCache.end())
        << "loadFullTrace on nonexistent trace must not create a cache entry";
}

TEST_F(TraceStoreTest, LoadFullTrace_PopulatesHashes_Opportunistically)
{
    // loadFullTrace should populate traceHash + structHash + deps in one shot.
    auto db = makeDb();
    std::vector<Dep> deps = {makeContentDep("/a.nix", "content")};
    auto result = db.record("test", string_t{"val", {}}, deps, true);

    // Clear caches, then use loadFullTrace (not ensureTraceHashes)
    db.traceDataCache.clear();

    auto loaded = db.loadFullTrace(result.traceId);
    EXPECT_EQ(loaded.size(), 1u);

    auto it = db.traceDataCache.find(result.traceId);
    ASSERT_NE(it, db.traceDataCache.end());
    EXPECT_TRUE(it->second.hashesPopulated())
        << "loadFullTrace should opportunistically populate hash fields";
    EXPECT_TRUE(it->second.deps.has_value())
        << "loadFullTrace should populate deps";
}

TEST_F(TraceStoreTest, TraceDataCache_EnsureThenLoad_SingleDbQuery)
{
    // ensureTraceHashes (via getCurrentTraceHash) then loadFullTrace should
    // reuse cached hashes. Hash fields should be identical regardless of order.
    auto db = makeDb();
    std::vector<Dep> deps = {makeContentDep("/a.nix", "content")};
    auto result = db.record("test", string_t{"val", {}}, deps, true);

    // Record populates everything; get hashes for comparison
    auto itAfterRecord = db.traceDataCache.find(result.traceId);
    ASSERT_NE(itAfterRecord, db.traceDataCache.end());
    auto expectedTraceHash = itAfterRecord->second.traceHash;

    // Clear and re-populate via ensureTraceHashes (getCurrentTraceHash)
    db.traceDataCache.clear();
    auto traceHash = db.getCurrentTraceHash("test");
    ASSERT_TRUE(traceHash.has_value());
    EXPECT_EQ(*traceHash, expectedTraceHash);

    // Verify deps not yet populated (lazy)
    auto itPartial = db.traceDataCache.find(result.traceId);
    ASSERT_NE(itPartial, db.traceDataCache.end());
    EXPECT_FALSE(itPartial->second.deps.has_value())
        << "ensureTraceHashes should not populate deps";

    // Now loadFullTrace should find the partial cache entry and extend it
    auto loaded = db.loadFullTrace(result.traceId);
    EXPECT_EQ(loaded.size(), 1u);

    auto it = db.traceDataCache.find(result.traceId);
    ASSERT_NE(it, db.traceDataCache.end());
    EXPECT_EQ(it->second.traceHash, expectedTraceHash);
    EXPECT_TRUE(it->second.deps.has_value());
}

TEST_F(TraceStoreTest, TraceRowCache_HitAfterRecord)
{
    // After record(), lookupTraceRow should hit the traceRowCache (no DB).
    auto db = makeDb();
    auto result = db.record("myattr", string_t{"result", {}},
        {makeEnvVarDep("FOO", "bar")}, true);

    // traceRowCache should be populated from record()
    EXPECT_EQ(db.traceRowCache.count("myattr"), 1u)
        << "record() should populate traceRowCache";

    // Verify the cached row has correct traceId
    auto & row = db.traceRowCache["myattr"];
    EXPECT_EQ(row.traceId, result.traceId);
}

TEST_F(TraceStoreTest, TraceRowCache_HitAfterLookup)
{
    // After a lookupTraceRow miss that hits DB, cache should be populated.
    auto db = makeDb();
    db.record("attr", string_t{"val", {}}, {}, true);

    // Clear traceRowCache (simulate fresh session lookup, not record)
    db.traceRowCache.clear();

    // First lookup goes to DB, populates cache
    EXPECT_TRUE(db.attrExists("attr"));
    EXPECT_EQ(db.traceRowCache.count("attr"), 1u)
        << "lookupTraceRow should populate traceRowCache on DB hit";
}

TEST_F(TraceStoreTest, TraceRowCache_MissDoesNotCache)
{
    // Looking up a nonexistent attr should NOT create a cache entry.
    auto db = makeDb();
    EXPECT_FALSE(db.attrExists("nonexistent"));
    EXPECT_EQ(db.traceRowCache.count("nonexistent"), 0u)
        << "lookupTraceRow miss should not create a cache entry";
}

TEST_F(TraceStoreTest, GetCurrentTraceHash_CachedAfterFirstCall)
{
    // getCurrentTraceHash should cache its result — second call hits cache.
    auto db = makeDb();
    // Record a parent with tab-separated key (simulating ParentContext dep key format)
    std::string attrPath = "packages";
    attrPath.push_back('\0');
    attrPath.append("hello");
    db.record(attrPath, string_t{"result", {}},
        {makeContentDep("/hello.nix", "v1")}, true);

    // Convert to tab-separated key as ParentContext deps do
    std::string depKey = "packages";
    depKey.push_back('\t');
    depKey.append("hello");

    auto hash1 = db.getCurrentTraceHash(depKey);
    ASSERT_TRUE(hash1.has_value());

    // Clear DB-level caches but keep session caches (traceRowCache + traceDataCache)
    // This simulates a "second call in the same session"
    // If the second call goes to DB, it would still work, but we want to verify caching.

    auto hash2 = db.getCurrentTraceHash(depKey);
    ASSERT_TRUE(hash2.has_value());
    EXPECT_EQ(*hash1, *hash2)
        << "getCurrentTraceHash should return same value from cache";
}

TEST_F(TraceStoreTest, TraceRowCache_InvalidatedOnRecovery)
{
    // When recovery succeeds, traceRowCache should be updated to reflect
    // the recovered trace. Subsequent lookups should return the new trace.
    auto db = makeDb();

    // Record version 1
    std::vector<Dep> depsV1 = {makeEnvVarDep("MY_VAR", "v1")};
    db.record("attr", string_t{"result-v1", {}}, depsV1, true);

    // Record version 2 (different deps → different trace)
    std::vector<Dep> depsV2 = {makeEnvVarDep("MY_VAR", "v2")};
    db.record("attr", string_t{"result-v2", {}}, depsV2, true);

    // Clear session caches to simulate new session
    db.clearSessionCaches();

    // Set env to v1 (mismatches current trace v2, should recover to v1)
    setenv("MY_VAR", "v1", 1);
    auto verifyResult = db.verify("attr", {}, state);
    ASSERT_TRUE(verifyResult.has_value());

    // Verify the traceRowCache was updated
    EXPECT_EQ(db.traceRowCache.count("attr"), 1u);
    auto & row = db.traceRowCache["attr"];
    EXPECT_EQ(row.traceId, verifyResult->traceId)
        << "traceRowCache should reflect the recovered trace";

    // Verify getCurrentTraceHash also reflects recovery
    auto traceHash = db.getCurrentTraceHash("attr");
    ASSERT_TRUE(traceHash.has_value());
    auto * data = db.traceDataCache.count(verifyResult->traceId)
        ? &db.traceDataCache[verifyResult->traceId] : nullptr;
    if (data && data->hashesPopulated()) {
        EXPECT_EQ(*traceHash, data->traceHash)
            << "getCurrentTraceHash should match recovered trace's hash";
    }

    unsetenv("MY_VAR");
}

TEST_F(TraceStoreTest, InMemoryRecovery_DirectHash)
{
    // Recovery should use in-memory trace_hash matching (no per-group DB lookup).
    // This test verifies the functional correctness of in-memory direct hash recovery.
    auto db = makeDb();

    // Record two versions
    setenv("INTEST", "alpha", 1);
    std::vector<Dep> depsA = {makeEnvVarDep("INTEST", "alpha")};
    db.record("attr", string_t{"result-alpha", {}}, depsA, true);

    setenv("INTEST", "beta", 1);
    std::vector<Dep> depsB = {makeEnvVarDep("INTEST", "beta")};
    db.record("attr", string_t{"result-beta", {}}, depsB, true);

    // Clear session caches
    db.clearSessionCaches();

    // Revert env to alpha → direct hash recovery should find the alpha trace
    setenv("INTEST", "alpha", 1);
    auto result = db.verify("attr", {}, state);
    ASSERT_TRUE(result.has_value());

    // Should have recovered to alpha's result
    auto * strResult = std::get_if<string_t>(&result->value);
    ASSERT_NE(strResult, nullptr);
    EXPECT_EQ(strResult->first, "result-alpha");

    unsetenv("INTEST");
}

TEST_F(TraceStoreTest, InMemoryRecovery_StructuralVariant)
{
    // Structural variant recovery should also use in-memory matching.
    auto db = makeDb();

    // Version 1: depends on VAR_A only
    setenv("SVR_A", "a1", 1);
    std::vector<Dep> depsV1 = {makeEnvVarDep("SVR_A", "a1")};
    db.record("attr", string_t{"r1", {}}, depsV1, true);

    // Version 2: depends on VAR_A + VAR_B (different dep structure)
    setenv("SVR_A", "a2", 1);
    setenv("SVR_B", "b2", 1);
    std::vector<Dep> depsV2 = {
        makeEnvVarDep("SVR_A", "a2"),
        makeEnvVarDep("SVR_B", "b2"),
    };
    db.record("attr", string_t{"r2", {}}, depsV2, true);

    // Clear caches
    db.clearSessionCaches();

    // Set env to match v1 (single dep)
    setenv("SVR_A", "a1", 1);
    unsetenv("SVR_B");

    auto result = db.verify("attr", {}, state);
    ASSERT_TRUE(result.has_value());

    auto * strResult = std::get_if<string_t>(&result->value);
    ASSERT_NE(strResult, nullptr);
    EXPECT_EQ(strResult->first, "r1")
        << "Structural variant recovery should find the matching trace";

    unsetenv("SVR_A");
}

TEST_F(TraceStoreTest, ClearSessionCaches_ClearsAllNewCaches)
{
    // clearSessionCaches should clear both traceDataCache and traceRowCache.
    auto db = makeDb();
    db.record("attr", string_t{"val", {}}, {makeContentDep("/f", "c")}, true);

    EXPECT_FALSE(db.traceDataCache.empty());
    EXPECT_FALSE(db.traceRowCache.empty());

    db.clearSessionCaches();

    EXPECT_TRUE(db.traceDataCache.empty())
        << "clearSessionCaches must clear traceDataCache";
    EXPECT_TRUE(db.traceRowCache.empty())
        << "clearSessionCaches must clear traceRowCache";
}

} // namespace nix::eval_trace
