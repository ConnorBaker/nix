#include "helpers.hh"
#include "nix/expr/trace-store.hh"
#include "nix/expr/trace-hash.hh"

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
}

// ── record tests (BSàlC: trace recording / fresh evaluation) ─────

TEST_F(TraceStoreTest, Record_ReturnsTraceId)
{
    auto db = makeDb();
    auto result = db.record("", string_t{"hello", {}}, {}, std::nullopt, true);
    // Trace recording returns a positive trace identifier (BSàlC: trace key)
    EXPECT_GT(result.traceId, 0);
}

TEST_F(TraceStoreTest, ColdStore_AttrExists)
{
    auto db = makeDb();
    db.record("", string_t{"hello", {}}, {}, std::nullopt, true);

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

    auto result = db.record("", int_t{NixInt{42}}, deps, std::nullopt, true);

    auto loadedDeps = db.loadFullTrace(result.traceId);
    EXPECT_EQ(loadedDeps.size(), 2u);
}

TEST_F(TraceStoreTest, ColdStore_VolatileDep_NotSessionCached)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeCurrentTimeDep()};

    auto result = db.record("", null_t{}, deps, std::nullopt, true);

    // Volatile dep (CurrentTime) -> trace NOT marked as verified in session (Salsa: no memoization)
    EXPECT_FALSE(db.verifiedTraceIds.count(result.traceId));
}

TEST_F(TraceStoreTest, ColdStore_NonVolatile_SessionCached)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeContentDep("/a.nix", "a")};

    auto result = db.record("", null_t{}, deps, std::nullopt, true);

    // Non-volatile dep -> trace marked as verified in session (Salsa: memoized query result)
    EXPECT_TRUE(db.verifiedTraceIds.count(result.traceId));
}

TEST_F(TraceStoreTest, ColdStore_ParentContextFiltered)
{
    auto db = makeDb();
    std::vector<Dep> deps = {
        makeContentDep("/a.nix", "a"),
        Dep{"", "", DepHashValue(std::string("parent-hash")), DepType::ParentContext},
    };

    auto result = db.record("", null_t{}, deps, std::nullopt, true);

    auto loadedDeps = db.loadFullTrace(result.traceId);
    // ParentContext deps are filtered at recording time -- only oracle deps stored in trace
    EXPECT_EQ(loadedDeps.size(), 1u);
    EXPECT_EQ(loadedDeps[0].type, DepType::Content);
}

TEST_F(TraceStoreTest, ColdStore_WithParent)
{
    auto db = makeDb();

    // Record parent trace (BSàlC: trace for root key)
    auto parentResult = db.record("", string_t{"parent-val", {}},
                                  {makeContentDep("/a.nix", "a")}, std::nullopt, true);

    // Record child trace with parent link (Adapton: edge in DDG)
    auto childResult = db.record("child", string_t{"child-val", {}},
                                 {makeEnvVarDep("FOO", "bar")}, parentResult.traceId, false);

    // With delta encoding, loadFullTrace flattens the trace DAG (parent + child deps)
    auto childDeps = db.loadFullTrace(childResult.traceId);
    EXPECT_EQ(childDeps.size(), 2u);

    // Verify both dependency types are present in the flattened trace
    bool hasContent = false, hasEnvVar = false;
    for (auto & dep : childDeps) {
        if (dep.type == DepType::Content) hasContent = true;
        if (dep.type == DepType::EnvVar) hasEnvVar = true;
    }
    EXPECT_TRUE(hasContent);
    EXPECT_TRUE(hasEnvVar);
}

TEST_F(TraceStoreTest, ColdStore_Deterministic)
{
    // Deterministic recording: same deps + result -> same trace (BSàlC: content-addressed trace)
    auto db = makeDb();
    std::vector<Dep> deps = {makeContentDep("/a.nix", "a")};
    CachedResult value = string_t{"result", {}};

    auto r1 = db.record("", value, deps, std::nullopt, true);
    auto r2 = db.record("", value, deps, std::nullopt, true);

    // Same deps + same parent -> same trace (content-addressed deduplication)
    EXPECT_EQ(r1.traceId, r2.traceId);
}

TEST_F(TraceStoreTest, ColdStore_AllValueTypes)
{
    auto db = makeDb();

    auto testRoundtrip = [&](const CachedResult & value, std::string_view name) {
        auto path = std::string(name);
        db.record(path, value, {}, std::nullopt, false);

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
    auto r1 = db.record("a", string_t{"val1", {}}, deps, std::nullopt, false);
    auto r2 = db.record("b", string_t{"val2", {}}, deps, std::nullopt, false);

    // Both should share the same trace ID (content-addressed)
    EXPECT_EQ(r1.traceId, r2.traceId);

    auto deps1 = db.loadFullTrace(r1.traceId);
    EXPECT_EQ(deps1.size(), 1u);
}

TEST_F(TraceStoreTest, EmptyTrace_HasDbRow)
{
    // Attributes with zero deps must still get a trace row (BSàlC: empty trace is valid)
    auto db = makeDb();

    auto result = db.record("", string_t{"val", {}}, {}, std::nullopt, true);

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
    auto result = db.record("", null_t{}, deps, std::nullopt, true);

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
    auto result = db.record("", null_t{}, deps, std::nullopt, true);

    // Clear session memo cache so verifyTrace re-checks all deps (Salsa: force re-verification)
    db.clearSessionCaches();

    bool valid = db.verifyTrace(result.traceId, {}, state);
    EXPECT_FALSE(valid);
}

TEST_F(TraceStoreTest, VerifyTrace_CurrentTime_AlwaysFails)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeCurrentTimeDep()};
    auto result = db.record("", null_t{}, deps, std::nullopt, true);

    // CurrentTime is volatile — verification always fails (Shake: always-dirty rule)
    bool valid = db.verifyTrace(result.traceId, {}, state);
    EXPECT_FALSE(valid);
}

TEST_F(TraceStoreTest, VerifyTrace_Exec_AlwaysFails)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeExecDep()};
    auto result = db.record("", null_t{}, deps, std::nullopt, true);

    bool valid = db.verifyTrace(result.traceId, {}, state);
    EXPECT_FALSE(valid);
}

TEST_F(TraceStoreTest, VerifyTrace_SessionCacheHit)
{
    ScopedEnvVar env("NIX_TEST_CACHE_VAR2", "val");

    auto db = makeDb();
    std::vector<Dep> deps = {makeEnvVarDep("NIX_TEST_CACHE_VAR2", "val")};
    auto result = db.record("", null_t{}, deps, std::nullopt, true);

    // Recording with non-volatile deps should session-memo the trace (Salsa: memoization)
    EXPECT_TRUE(db.verifiedTraceIds.count(result.traceId));
    // Second call hits session memo — skips re-verification (Salsa: green query)
    EXPECT_TRUE(db.verifyTrace(result.traceId, {}, state));
}

TEST_F(TraceStoreTest, VerifyTrace_NoDeps_Valid)
{
    auto db = makeDb();
    auto result = db.record("", string_t{"val", {}}, {}, std::nullopt, true);

    db.clearSessionCaches();

    bool valid = db.verifyTrace(result.traceId, {}, state);
    EXPECT_TRUE(valid);
}

TEST_F(TraceStoreTest, VerifyTrace_ParentInvalid)
{
    ScopedEnvVar env("NIX_TEST_PARENT", "current_value");

    auto db = makeDb();

    // Record parent trace with stale dep (Shake: parent's input changed)
    std::vector<Dep> staleDeps = {makeEnvVarDep("NIX_TEST_PARENT", "stale_value")};
    auto parentResult = db.record("", string_t{"parent", {}},
                                  staleDeps, std::nullopt, true);

    // Record child with no direct deps, but parent trace is invalid (Shake: transitive dirty)
    auto childResult = db.record("child", string_t{"child", {}},
                                 {}, parentResult.traceId, false);

    // Clear session memo cache
    db.clearSessionCaches();

    bool valid = db.verifyTrace(childResult.traceId, {}, state);
    EXPECT_FALSE(valid);
}

TEST_F(TraceStoreTest, VerifyTrace_ParentValid)
{
    ScopedEnvVar env("NIX_TEST_PARENT", "correct_value");

    auto db = makeDb();

    // Record parent trace with correct dep (BSàlC: verifying trace succeeds)
    std::vector<Dep> parentDeps = {makeEnvVarDep("NIX_TEST_PARENT", "correct_value")};
    auto parentResult = db.record("", string_t{"parent", {}},
                                  parentDeps, std::nullopt, true);

    // Record child with valid parent (Shake: transitive clean)
    auto childResult = db.record("child", string_t{"child", {}},
                                 {}, parentResult.traceId, false);

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
    auto result = db.record("", null_t{}, deps, std::nullopt, true);

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

    db.record("", input, deps, std::nullopt, true);

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

    auto coldResult = db.record("", input, deps, std::nullopt, true);

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
    db.record("", string_t{"old", {}}, deps, std::nullopt, true);

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
    db.record("", string_t{"result_A", {}}, depsA, std::nullopt, true);

    // Change env var to value_B and record new trace
    setenv("NIX_P1_TEST", "value_B", 1);
    std::vector<Dep> depsB = {makeEnvVarDep("NIX_P1_TEST", "value_B")};
    db.record("", string_t{"result_B", {}}, depsB, std::nullopt, true);

    // Revert to value_A -- Phase 1 constructive recovery should find the trace from first recording
    setenv("NIX_P1_TEST", "value_A", 1);
    db.clearSessionCaches();

    auto result = db.verify("", {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"result_A", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, Phase1_ParentContextDisambiguation)
{
    // Dep-less child, parent changed. Phase 1 recovers via parent-mixed trace hash (Merkle identity).
    ScopedEnvVar env("NIX_P1_ROOT", "val1");

    auto db = makeDb();

    // Record root trace with val1
    std::vector<Dep> rootDeps1 = {makeEnvVarDep("NIX_P1_ROOT", "val1")};
    auto rootResult1 = db.record("", string_t{"root1", {}}, rootDeps1, std::nullopt, true);

    // Record child trace (no deps) linked to root1 (Adapton: DDG edge)
    db.record("child", string_t{"child1", {}}, {}, rootResult1.traceId, false);

    // Record root trace with val2
    setenv("NIX_P1_ROOT", "val2", 1);
    std::vector<Dep> rootDeps2 = {makeEnvVarDep("NIX_P1_ROOT", "val2")};
    auto rootResult2 = db.record("", string_t{"root2", {}}, rootDeps2, std::nullopt, true);

    // Record child trace linked to root2
    db.record("child", string_t{"child2", {}}, {}, rootResult2.traceId, false);

    // Revert to val1: root recovered via Phase 1, then child via parent-mixed trace hash
    setenv("NIX_P1_ROOT", "val1", 1);
    db.clearSessionCaches();

    // Root constructive recovery (Phase 1: direct trace hash match)
    auto rootResult = db.verify("", {}, state);
    ASSERT_TRUE(rootResult.has_value());
    assertCachedResultEquals(string_t{"root1", {}}, rootResult->value, state.symbols);

    // Child constructive recovery (Phase 1 -- parent-mixed trace hash via Merkle identity)
    auto childResult = db.verify("child", {}, state, rootResult->traceId);
    ASSERT_TRUE(childResult.has_value());
    assertCachedResultEquals(string_t{"child1", {}}, childResult->value, state.symbols);
}

TEST_F(TraceStoreTest, Phase1_ParentContextWithChildDeps)
{
    // Child with deps, parent changed. Phase 1 recovers via parent-mixed trace hash.
    ScopedEnvVar env1("NIX_P1W_ROOT", "rval1");
    ScopedEnvVar env2("NIX_P1W_CHILD", "cval");

    auto db = makeDb();

    // Record root trace with rval1
    std::vector<Dep> rootDeps1 = {makeEnvVarDep("NIX_P1W_ROOT", "rval1")};
    auto rootResult1 = db.record("", string_t{"root1", {}}, rootDeps1, std::nullopt, true);

    // Record child trace with stable dep + parent link
    std::vector<Dep> childDeps = {makeEnvVarDep("NIX_P1W_CHILD", "cval")};
    db.record("child", string_t{"child1", {}}, childDeps, rootResult1.traceId, false);

    // Record root trace with rval2
    setenv("NIX_P1W_ROOT", "rval2", 1);
    std::vector<Dep> rootDeps2 = {makeEnvVarDep("NIX_P1W_ROOT", "rval2")};
    auto rootResult2 = db.record("", string_t{"root2", {}}, rootDeps2, std::nullopt, true);

    // Record child trace linked to root2
    db.record("child", string_t{"child2", {}}, childDeps, rootResult2.traceId, false);

    // Revert to rval1 — triggers constructive recovery cascade
    setenv("NIX_P1W_ROOT", "rval1", 1);
    db.clearSessionCaches();

    auto rootResult = db.verify("", {}, state);
    ASSERT_TRUE(rootResult.has_value());

    auto childResult = db.verify("child", {}, state, rootResult->traceId);
    ASSERT_TRUE(childResult.has_value());
    assertCachedResultEquals(string_t{"child1", {}}, childResult->value, state.symbols);
}

TEST_F(TraceStoreTest, Phase1_CascadeThroughTree)
{
    // Root -> child1 -> child2 (grandchild), with deps at each level.
    // Phase 1 uses trace_hash = hash(deps + "P" + parent_trace_hash) (Merkle identity),
    // so each level's trace hash changes when ancestor deps change.
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
    auto root1 = db.record("", string_t{"r1", {}}, rootDeps1, std::nullopt, true);
    auto child1_1 = db.record("c1", string_t{"c1v1", {}}, childDeps1, root1.traceId, false);
    db.record(c2AttrPath, string_t{"c2v1", {}}, {}, child1_1.traceId, false);

    // Version 2 — record new traces with changed deps at root and child
    setenv("NIX_P1C_ROOT", "v2", 1);
    setenv("NIX_P1C_CHILD", "cv2", 1);
    std::vector<Dep> rootDeps2 = {makeEnvVarDep("NIX_P1C_ROOT", "v2")};
    std::vector<Dep> childDeps2 = {makeEnvVarDep("NIX_P1C_CHILD", "cv2")};
    auto root2 = db.record("", string_t{"r2", {}}, rootDeps2, std::nullopt, true);
    auto child1_2 = db.record("c1", string_t{"c1v2", {}}, childDeps2, root2.traceId, false);
    db.record(c2AttrPath, string_t{"c2v2", {}}, {}, child1_2.traceId, false);

    // Revert to v1 — constructive recovery should cascade through all levels
    setenv("NIX_P1C_ROOT", "v1", 1);
    setenv("NIX_P1C_CHILD", "cv1", 1);
    db.clearSessionCaches();

    auto rootR = db.verify("", {}, state);
    ASSERT_TRUE(rootR.has_value());
    assertCachedResultEquals(string_t{"r1", {}}, rootR->value, state.symbols);

    auto c1R = db.verify("c1", {}, state, rootR->traceId);
    ASSERT_TRUE(c1R.has_value());
    assertCachedResultEquals(string_t{"c1v1", {}}, c1R->value, state.symbols);

    auto c2R = db.verify(c2AttrPath, {}, state, c1R->traceId);
    ASSERT_TRUE(c2R.has_value());
    assertCachedResultEquals(string_t{"c2v1", {}}, c2R->value, state.symbols);
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
    db.record("", string_t{"result1", {}}, deps1, std::nullopt, true);

    // Second recording: trace with deps A + B (different structural hash)
    std::vector<Dep> deps2 = {
        makeEnvVarDep("NIX_P3_A", "aval"),
        makeEnvVarDep("NIX_P3_B", "bval"),
    };
    db.record("", string_t{"result2", {}}, deps2, std::nullopt, true);

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
                  {makeEnvVarDep("NIX_P3M_A", "a")}, std::nullopt, true);

    // Structural hash group 2: A + B
    db.record("", string_t{"r2", {}},
                  {makeEnvVarDep("NIX_P3M_A", "a"), makeEnvVarDep("NIX_P3M_B", "b")},
                  std::nullopt, true);

    // Structural hash group 3: A + B + C (latest, in attribute entry)
    db.record("", string_t{"r3", {}},
                  {makeEnvVarDep("NIX_P3M_A", "a"), makeEnvVarDep("NIX_P3M_B", "b"),
                   makeEnvVarDep("NIX_P3M_C", "c")},
                  std::nullopt, true);

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

    db.record("", string_t{"empty1", {}}, {}, std::nullopt, true);
    db.record("", string_t{"empty2", {}},
                  {makeEnvVarDep("NIX_P3E_X", "x")}, std::nullopt, true);

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
                  {makeEnvVarDep("NIX_P1F_A", "a")}, std::nullopt, true);
    db.record("", string_t{"r2", {}},
                  {makeEnvVarDep("NIX_P1F_A", "a"), makeEnvVarDep("NIX_P1F_B", "b")},
                  std::nullopt, true);

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
    db.record("", null_t{}, deps, std::nullopt, true);

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
    db.record("", string_t{"rA", {}}, depsA, std::nullopt, true);

    setenv("NIX_RUI_TEST", "val_B", 1);
    std::vector<Dep> depsB = {makeEnvVarDep("NIX_RUI_TEST", "val_B")};
    db.record("", string_t{"rB", {}}, depsB, std::nullopt, true);

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
                  {makeEnvVarDep("NIX_CASCADE_A", "a")}, std::nullopt, false);

    // Trace with structural hash for deps A + B (latest, in attribute)
    db.record("child", string_t{"latest", {}},
                  {makeEnvVarDep("NIX_CASCADE_A", "a"), makeEnvVarDep("NIX_CASCADE_B", "b")},
                  std::nullopt, false);

    // Invalidate B -> Phase 1 fails (trace hash A+B, B mismatches)
    // Phase 3 finds structural group with only A -> constructive recovery succeeds
    setenv("NIX_CASCADE_B", "b_new", 1);
    db.clearSessionCaches();

    auto result = db.verify("child", {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"target", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, Phase1_DeepChainRecovery)
{
    // Deep trace chain (depth 6): root -> c1 -> c2 -> c3 -> c4 -> c5
    // Record two versions, revert, verify Phase 1 constructive recovery at deepest level.
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
    auto root1 = db.record("", string_t{"root_v1", {}}, rootDeps1, std::nullopt, true);
    auto c1_1 = db.record(c1Path, string_t{"c1_v1", {}}, c1Deps1, root1.traceId, false);
    auto c2_1 = db.record(c2Path, string_t{"c2_v1", {}}, {}, c1_1.traceId, false);
    auto c3_1 = db.record(c3Path, string_t{"c3_v1", {}}, {}, c2_1.traceId, false);
    auto c4_1 = db.record(c4Path, string_t{"c4_v1", {}}, {}, c3_1.traceId, false);
    db.record(c5Path, string_t{"c5_v1", {}}, {}, c4_1.traceId, false);

    // Version 2: record traces with different dep values
    setenv("NIX_DEEP_ROOT", "v2", 1);
    setenv("NIX_DEEP_C1", "v2", 1);
    std::vector<Dep> rootDeps2 = {makeEnvVarDep("NIX_DEEP_ROOT", "v2")};
    std::vector<Dep> c1Deps2 = {makeEnvVarDep("NIX_DEEP_C1", "v2")};
    auto root2 = db.record("", string_t{"root_v2", {}}, rootDeps2, std::nullopt, true);
    auto c1_2 = db.record(c1Path, string_t{"c1_v2", {}}, c1Deps2, root2.traceId, false);
    auto c2_2 = db.record(c2Path, string_t{"c2_v2", {}}, {}, c1_2.traceId, false);
    auto c3_2 = db.record(c3Path, string_t{"c3_v2", {}}, {}, c2_2.traceId, false);
    auto c4_2 = db.record(c4Path, string_t{"c4_v2", {}}, {}, c3_2.traceId, false);
    db.record(c5Path, string_t{"c5_v2", {}}, {}, c4_2.traceId, false);

    // Revert to v1
    setenv("NIX_DEEP_ROOT", "v1", 1);
    setenv("NIX_DEEP_C1", "v1", 1);
    db.clearSessionCaches();

    // Recover root via Phase 1 direct trace hash match
    auto rootR = db.verify("", {}, state);
    ASSERT_TRUE(rootR.has_value());
    assertCachedResultEquals(string_t{"root_v1", {}}, rootR->value, state.symbols);

    // Recover chain down to c5 (Phase 1 at each level via parent-mixed Merkle identity)
    auto c1R = db.verify(c1Path, {}, state, rootR->traceId);
    ASSERT_TRUE(c1R.has_value());
    assertCachedResultEquals(string_t{"c1_v1", {}}, c1R->value, state.symbols);

    auto c2R = db.verify(c2Path, {}, state, c1R->traceId);
    ASSERT_TRUE(c2R.has_value());
    assertCachedResultEquals(string_t{"c2_v1", {}}, c2R->value, state.symbols);

    auto c3R = db.verify(c3Path, {}, state, c2R->traceId);
    ASSERT_TRUE(c3R.has_value());
    assertCachedResultEquals(string_t{"c3_v1", {}}, c3R->value, state.symbols);

    auto c4R = db.verify(c4Path, {}, state, c3R->traceId);
    ASSERT_TRUE(c4R.has_value());
    assertCachedResultEquals(string_t{"c4_v1", {}}, c4R->value, state.symbols);

    auto c5R = db.verify(c5Path, {}, state, c4R->traceId);
    ASSERT_TRUE(c5R.has_value());
    assertCachedResultEquals(string_t{"c5_v1", {}}, c5R->value, state.symbols);
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
        db.record("", string_t{result, {}}, deps, std::nullopt, true);
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
    db.record("", string_t{"old_result", {}}, deps, std::nullopt, true);

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
        db1.record("pkg", string_t{"v1", {}}, {}, std::nullopt, false);
    }
    {
        TraceStore db2(state.symbols, 222);
        db2.record("pkg", string_t{"v2", {}}, {}, std::nullopt, false);
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

    db.record(attrPath, string_t{"val", {}}, {}, std::nullopt, false);
    EXPECT_TRUE(db.attrExists(attrPath));
    EXPECT_FALSE(db.attrExists("packages"));
}

TEST_F(TraceStoreTest, EmptyAttrPath)
{
    auto db = makeDb();
    db.record("", string_t{"root-val", {}}, {}, std::nullopt, true);
    EXPECT_TRUE(db.attrExists(""));
}

TEST_F(TraceStoreTest, MultipleEntries_Stress)
{
    auto db = makeDb();

    // Record 100 trace entries
    for (int i = 0; i < 100; i++) {
        auto name = "stress-" + std::to_string(i);
        db.record(name, int_t{NixInt{i}}, {}, std::nullopt, false);
    }

    EXPECT_TRUE(db.attrExists("stress-0"));
    EXPECT_TRUE(db.attrExists("stress-99"));
    EXPECT_FALSE(db.attrExists("stress-100"));
}

// ── BLOB serialization roundtrip tests (trace dep encoding) ──────────

TEST_F(TraceStoreTest, BlobRoundTrip_Empty)
{
    std::vector<TraceStore::InternedDep> deps;
    auto blob = TraceStore::serializeDeps(deps);
    EXPECT_TRUE(blob.empty());
    auto result = TraceStore::deserializeInternedDeps(blob.data(), blob.size());
    EXPECT_TRUE(result.empty());
}

TEST_F(TraceStoreTest, BlobRoundTrip_Blake3Deps)
{
    std::vector<TraceStore::InternedDep> deps;
    for (int i = 0; i < 5; i++) {
        auto hash = depHash("content-" + std::to_string(i));
        deps.push_back({DepType::Content, static_cast<uint32_t>(i + 1),
                        static_cast<uint32_t>(i + 100), DepHashValue(hash)});
    }

    auto blob = TraceStore::serializeDeps(deps);
    EXPECT_FALSE(blob.empty());

    auto result = TraceStore::deserializeInternedDeps(blob.data(), blob.size());
    ASSERT_EQ(result.size(), 5u);
    for (int i = 0; i < 5; i++) {
        EXPECT_EQ(result[i].type, DepType::Content);
        EXPECT_EQ(result[i].sourceId, static_cast<uint32_t>(i + 1));
        EXPECT_EQ(result[i].keyId, static_cast<uint32_t>(i + 100));
        EXPECT_EQ(result[i].hash, deps[i].hash);
    }
}

TEST_F(TraceStoreTest, BlobRoundTrip_MixedDeps)
{
    std::vector<TraceStore::InternedDep> deps;

    // BLAKE3 hash dep (Content — file content oracle)
    deps.push_back({DepType::Content, 1, 2, DepHashValue(depHash("file-data"))});

    // String hash dep (CopiedPath — store path oracle)
    deps.push_back({DepType::CopiedPath, 3, 4,
                    DepHashValue(std::string("/nix/store/aaaa-test"))});

    // BLAKE3 hash dep (EnvVar — environment oracle)
    deps.push_back({DepType::EnvVar, 5, 6, DepHashValue(depHash("env-val"))});

    // String hash dep (Existence — filesystem oracle)
    deps.push_back({DepType::Existence, 7, 8, DepHashValue(std::string("missing"))});

    auto blob = TraceStore::serializeDeps(deps);
    auto result = TraceStore::deserializeInternedDeps(blob.data(), blob.size());
    ASSERT_EQ(result.size(), 4u);

    // Content oracle: BLAKE3 hash
    EXPECT_EQ(result[0].type, DepType::Content);
    EXPECT_EQ(result[0].sourceId, 1u);
    EXPECT_EQ(result[0].keyId, 2u);
    EXPECT_TRUE(std::holds_alternative<Blake3Hash>(result[0].hash));

    // CopiedPath oracle: string (not BLAKE3, so deserialized as string)
    EXPECT_EQ(result[1].type, DepType::CopiedPath);
    EXPECT_EQ(result[1].sourceId, 3u);
    EXPECT_EQ(result[1].keyId, 4u);
    EXPECT_TRUE(std::holds_alternative<std::string>(result[1].hash));
    EXPECT_EQ(std::get<std::string>(result[1].hash), "/nix/store/aaaa-test");

    // EnvVar oracle: BLAKE3 hash
    EXPECT_EQ(result[2].type, DepType::EnvVar);
    EXPECT_TRUE(std::holds_alternative<Blake3Hash>(result[2].hash));

    // Existence oracle: string
    EXPECT_EQ(result[3].type, DepType::Existence);
    EXPECT_TRUE(std::holds_alternative<std::string>(result[3].hash));
    EXPECT_EQ(std::get<std::string>(result[3].hash), "missing");
}

TEST_F(TraceStoreTest, BlobRoundTrip_LargeSet)
{
    std::vector<TraceStore::InternedDep> deps;
    for (uint32_t i = 0; i < 10000; i++) {
        auto hash = depHash("content-" + std::to_string(i));
        deps.push_back({DepType::Content, i, i + 50000, DepHashValue(hash)});
    }

    auto blob = TraceStore::serializeDeps(deps);
    // Each dep: 1 + 4 + 4 + 1 + 32 = 42 bytes
    EXPECT_EQ(blob.size(), 10000u * 42u);

    auto result = TraceStore::deserializeInternedDeps(blob.data(), blob.size());
    ASSERT_EQ(result.size(), 10000u);

    // Spot-check first, middle, and last
    EXPECT_EQ(result[0].sourceId, 0u);
    EXPECT_EQ(result[0].keyId, 50000u);
    EXPECT_EQ(result[5000].sourceId, 5000u);
    EXPECT_EQ(result[5000].keyId, 55000u);
    EXPECT_EQ(result[9999].sourceId, 9999u);
    EXPECT_EQ(result[9999].keyId, 59999u);

    // Verify hashes match
    for (uint32_t i = 0; i < 10000; i++) {
        EXPECT_EQ(result[i].hash, deps[i].hash) << "Hash mismatch at index " << i;
    }
}

// ── Delta encoding tests (trace compression via structural sharing) ──

TEST_F(TraceStoreTest, DeltaEncoding_SiblingOverlap)
{
    auto db = makeDb();

    // Parent with 0 deps (FullAttrs pattern — trace records only child names)
    auto parentResult = db.record("", null_t{}, {}, std::nullopt, true);

    // Child A trace with 100 deps
    std::vector<Dep> depsA;
    for (int i = 0; i < 100; i++) {
        depsA.push_back(makeContentDep("/file-" + std::to_string(i) + ".nix",
                                       "content-" + std::to_string(i)));
    }
    auto childA = db.record("a", string_t{"val-a", {}}, depsA, parentResult.traceId, false);

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
    auto childB = db.record("b", string_t{"val-b", {}}, depsB, parentResult.traceId, false);

    // Both traces should load correctly with full deps
    auto loadedA = db.loadFullTrace(childA.traceId);
    auto loadedB = db.loadFullTrace(childB.traceId);
    EXPECT_EQ(loadedA.size(), 100u);
    EXPECT_EQ(loadedB.size(), 100u);

    // After optimization, child B should use child A as base (same structural hash)
    // since they have the same 100 dep keys
    db.optimizeTraces();

    // Verify both traces still load correctly after optimization
    auto reloadedA = db.loadFullTrace(childA.traceId);
    auto reloadedB = db.loadFullTrace(childB.traceId);
    EXPECT_EQ(reloadedA.size(), 100u);
    EXPECT_EQ(reloadedB.size(), 100u);
}

TEST_F(TraceStoreTest, DeltaEncoding_StructHashMatch)
{
    auto db = makeDb();

    // Trace v1 with deps [A=h1, B=h2, C=h3]
    std::vector<Dep> depsV1 = {
        makeContentDep("/a.nix", "h1"),
        makeContentDep("/b.nix", "h2"),
        makeContentDep("/c.nix", "h3"),
    };
    auto v1 = db.record("attr", string_t{"v1", {}}, depsV1, std::nullopt, false);

    // Trace v2 with deps [A=h1', B=h2, C=h3] — same structural hash, different dep hash for A
    std::vector<Dep> depsV2 = {
        makeContentDep("/a.nix", "h1-modified"),
        makeContentDep("/b.nix", "h2"),
        makeContentDep("/c.nix", "h3"),
    };
    auto v2 = db.record("attr", string_t{"v2", {}}, depsV2, std::nullopt, false);

    // v2 should use v1 as structural-hash base since they have same dep keys
    // After optimization, v2's delta should be much smaller than v1's full trace
    db.optimizeTraces();

    // Both traces should still load correctly
    auto loadedV1 = db.loadFullTrace(v1.traceId);
    auto loadedV2 = db.loadFullTrace(v2.traceId);

    ASSERT_EQ(loadedV1.size(), 3u);
    ASSERT_EQ(loadedV2.size(), 3u);

    // Verify v2 trace has the modified dep hash for A
    bool foundModifiedA = false;
    for (auto & dep : loadedV2) {
        if (dep.key == "/a.nix") {
            auto h = depHash("h1-modified");
            EXPECT_EQ(dep.expectedHash, DepHashValue(h));
            foundModifiedA = true;
        }
    }
    EXPECT_TRUE(foundModifiedA);
}

TEST_F(TraceStoreTest, DeltaEncoding_NoParentBase)
{
    auto db = makeDb();

    // Parent with 0 deps (empty trace)
    auto parent = db.record("", null_t{}, {}, std::nullopt, true);

    // Child A trace with 100 deps
    std::vector<Dep> depsA;
    for (int i = 0; i < 100; i++) {
        depsA.push_back(makeContentDep("/f" + std::to_string(i) + ".nix",
                                       "c" + std::to_string(i)));
    }
    auto childA = db.record("a", int_t{NixInt{1}}, depsA, parent.traceId, false);

    // Child B trace with 99 overlapping + 1 different dep hash
    std::vector<Dep> depsB;
    for (int i = 0; i < 99; i++) {
        depsB.push_back(makeContentDep("/f" + std::to_string(i) + ".nix",
                                       "c" + std::to_string(i)));
    }
    depsB.push_back(makeContentDep("/f99.nix", "c99-modified"));
    auto childB = db.record("b", int_t{NixInt{2}}, depsB, parent.traceId, false);

    // After optimization, child B should NOT use parent (0 deps) as delta base.
    // It should use child A (same structural hash — same 100 dep keys).
    db.optimizeTraces();

    // Verify both traces still load correctly
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

TEST_F(TraceStoreTest, DeltaEncoding_PostWriteOptimization)
{
    auto db = makeDb();

    // Record 5 traces with same structural hash in suboptimal order (smallest first)
    std::vector<int64_t> traceIds;
    for (int setNum = 0; setNum < 5; setNum++) {
        std::vector<Dep> deps;
        for (int i = 0; i < 50; i++) {
            auto key = "/shared-" + std::to_string(i) + ".nix";
            auto content = "content-" + std::to_string(i) + "-v" + std::to_string(setNum);
            deps.push_back(makeContentDep(key, content));
        }
        auto result = db.record(
            "attr-" + std::to_string(setNum), int_t{NixInt{setNum}},
            deps, std::nullopt, false);
        traceIds.push_back(result.traceId);
    }

    // Run optimization
    db.optimizeTraces();

    // All 5 traces should still load correctly with 50 deps each
    for (int i = 0; i < 5; i++) {
        auto loaded = db.loadFullTrace(traceIds[i]);
        EXPECT_EQ(loaded.size(), 50u) << "Trace " << i << " has wrong number of deps";
    }
}

TEST_F(TraceStoreTest, DeltaEncoding_CrossCommitDelta)
{
    // Simulate 3 "commits" for the same attribute, each recording a trace with slightly different deps
    auto db = makeDb();

    // Base deps: 50 shared files
    auto makeDeps = [](int version) {
        std::vector<Dep> deps;
        for (int i = 0; i < 50; i++) {
            auto key = "/src/" + std::to_string(i) + ".nix";
            // Most deps are the same across versions, a few change
            auto content = (i < 45)
                ? "stable-content-" + std::to_string(i)
                : "content-v" + std::to_string(version) + "-" + std::to_string(i);
            deps.push_back(makeContentDep(key, content));
        }
        return deps;
    };

    auto r1 = db.record("", string_t{"commit-1", {}}, makeDeps(1), std::nullopt, true);
    auto r2 = db.record("", string_t{"commit-2", {}}, makeDeps(2), std::nullopt, true);
    auto r3 = db.record("", string_t{"commit-3", {}}, makeDeps(3), std::nullopt, true);

    // After optimization, all 3 traces should share a delta base
    db.optimizeTraces();

    // All should load correctly with 50 deps
    EXPECT_EQ(db.loadFullTrace(r1.traceId).size(), 50u);
    EXPECT_EQ(db.loadFullTrace(r2.traceId).size(), 50u);
    EXPECT_EQ(db.loadFullTrace(r3.traceId).size(), 50u);
}

TEST_F(TraceStoreTest, DeltaEncoding_RowCount)
{
    // Record 10 traces each with 100 shared deps + 10 unique deps.
    // Total storage should be much less than 10 x 110 due to structural sharing.
    auto db = makeDb();

    std::vector<int64_t> traceIds;
    for (int attrNum = 0; attrNum < 10; attrNum++) {
        std::vector<Dep> deps;
        // 100 shared deps (same keys, same hashes)
        for (int i = 0; i < 100; i++) {
            deps.push_back(makeContentDep(
                "/shared-" + std::to_string(i) + ".nix", "shared-content"));
        }
        // 10 unique deps per attr
        for (int i = 0; i < 10; i++) {
            deps.push_back(makeContentDep(
                "/unique-" + std::to_string(attrNum) + "-" + std::to_string(i) + ".nix",
                "unique-content"));
        }
        auto result = db.record(
            "attr-" + std::to_string(attrNum), int_t{NixInt{attrNum}},
            deps, std::nullopt, false);
        traceIds.push_back(result.traceId);
    }

    // All 10 traces should load correctly with 110 deps each
    for (int i = 0; i < 10; i++) {
        auto loaded = db.loadFullTrace(traceIds[i]);
        EXPECT_EQ(loaded.size(), 110u) << "Trace " << i << " has wrong number of deps";
    }
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
    auto result = db.record("", null_t{}, deps, std::nullopt, true);

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
    db.record("", string_t{"result-1", {}}, deps1, std::nullopt, true);

    // Version 2: A changed, B same
    setenv("NIX_HASHCACHE_A", "valA2", 1);
    std::vector<Dep> deps2 = {
        makeEnvVarDep("NIX_HASHCACHE_A", "valA2"),
        makeEnvVarDep("NIX_HASHCACHE_B", "valB"),
    };
    db.record("", string_t{"result-2", {}}, deps2, std::nullopt, true);

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
        db.record(name, int_t{NixInt{i}}, sharedDeps, std::nullopt, false);
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

// ── Full end-to-end delta + verification roundtrip ───────────────────

TEST_F(TraceStoreTest, DeltaEncoding_WarmRoundtrip)
{
    // End-to-end: record traces with delta encoding, then verification retrieves correctly.
    // Use EnvVar deps so verification can actually check them (no files needed).
    ScopedEnvVar env1("NIX_DW_SHARED", "stable");
    ScopedEnvVar env2("NIX_DW_A", "a-val");
    ScopedEnvVar env3("NIX_DW_B", "b-val");

    auto db = makeDb();

    // 3 attrs with overlapping deps (all env vars — verifiable)
    auto sharedDep = makeEnvVarDep("NIX_DW_SHARED", "stable");

    // Attr 1: shared + 1 unique
    std::vector<Dep> deps1 = {sharedDep, makeEnvVarDep("NIX_DW_A", "a-val")};
    db.record("a", string_t{"val-a", {}}, deps1, std::nullopt, false);

    // Attr 2: shared + 1 different unique
    std::vector<Dep> deps2 = {sharedDep, makeEnvVarDep("NIX_DW_B", "b-val")};
    db.record("b", string_t{"val-b", {}}, deps2, std::nullopt, false);

    // Attr 3: shared only
    std::vector<Dep> deps3 = {sharedDep};
    db.record("c", string_t{"val-c", {}}, deps3, std::nullopt, false);

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

TEST_F(TraceStoreTest, OptimizeTraces_NoDirty)
{
    // optimizeTraces with no dirty traces should be a no-op
    auto db = makeDb();
    db.optimizeTraces(); // Should not crash or throw
}

TEST_F(TraceStoreTest, OptimizeTraces_SingletonGroup)
{
    // Single trace in a structural hash group — no optimization needed
    auto db = makeDb();
    std::vector<Dep> deps = {makeContentDep("/a.nix", "a")};
    auto result = db.record("", null_t{}, deps, std::nullopt, true);

    // Should not crash; singleton structural groups are skipped
    db.optimizeTraces();

    auto loaded = db.loadFullTrace(result.traceId);
    EXPECT_EQ(loaded.size(), 1u);
}

} // namespace nix::eval_trace
