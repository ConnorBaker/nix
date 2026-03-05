#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/expr/eval-trace/deps/hash.hh"

#include <algorithm>
#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

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
    EXPECT_EQ(depTypeName(DepType::GitIdentity), "gitIdentity");
}

// ── record tests (BSàlC: trace recording / fresh evaluation) ─────

TEST_F(TraceStoreTest, Record_ReturnsTraceId)
{
    auto db = makeDb();
    auto result = db.record(rootPath(), string_t{"hello", {}}, {}, true);
    // Trace recording returns a positive trace identifier (BSàlC: trace key)
    EXPECT_GT(result.traceId.value, 0u);
}

TEST_F(TraceStoreTest, ColdStore_AttrExists)
{
    auto db = makeDb();
    db.record(rootPath(), string_t{"hello", {}}, {}, true);

    EXPECT_TRUE(db.attrExists(rootPath()));
    EXPECT_FALSE(db.attrExists(vpath({"nonexistent"})));
}

TEST_F(TraceStoreTest, ColdStore_WithDeps)
{
    auto db = makeDb();
    std::vector<Dep> deps = {
        makeContentDep(pools(), "/a.nix", "a"),
        makeEnvVarDep(pools(), "HOME", "/home"),
    };

    auto result = db.record(rootPath(), int_t{NixInt{42}}, deps, true);

    auto loadedDeps = db.loadFullTrace(result.traceId);
    EXPECT_EQ(loadedDeps.size(), 2u);
}

TEST_F(TraceStoreTest, ColdStore_VolatileDep_NotSessionCached)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeCurrentTimeDep(pools())};

    auto result = db.record(rootPath(), null_t{}, deps, true);

    // Volatile dep (CurrentTime) -> trace NOT marked as verified in session (Salsa: no memoization)
    EXPECT_FALSE(db.verifiedTraceIds.count(result.traceId));
}

TEST_F(TraceStoreTest, ColdStore_NonVolatile_SessionCached)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeContentDep(pools(), "/a.nix", "a")};

    auto result = db.record(rootPath(), null_t{}, deps, true);

    // Non-volatile dep -> trace marked as verified in session (Salsa: memoized query result)
    EXPECT_TRUE(db.verifiedTraceIds.count(result.traceId));
}

TEST_F(TraceStoreTest, ColdStore_ParentContextStored)
{
    auto db = makeDb();
    std::vector<Dep> deps = {
        makeContentDep(pools(), "/a.nix", "a"),
        Dep::makeParentContext(AttrPathId{}, DepHashValue(std::string("parent-hash"))),
    };

    auto result = db.record(rootPath(), null_t{}, deps, true);

    auto loadedDeps = db.loadFullTrace(result.traceId);
    // ParentContext deps are now stored (dep separation: children reference parent result hash)
    EXPECT_EQ(loadedDeps.size(), 2u);
}

TEST_F(TraceStoreTest, ColdStore_WithParent)
{
    auto db = makeDb();

    // Record parent trace (BSàlC: trace for root key)
    db.record(rootPath(), string_t{"parent-val", {}},
              {makeContentDep(pools(), "/a.nix", "a")}, true);

    // Record child trace — own deps only, no parent dep inheritance
    auto childResult = db.record(vpath({"child"}), string_t{"child-val", {}},
                                 {makeEnvVarDep(pools(), "FOO", "bar")}, false);

    // loadFullTrace returns only the child's own deps (no parent merging)
    auto childDeps = db.loadFullTrace(childResult.traceId);
    EXPECT_EQ(childDeps.size(), 1u);
    EXPECT_EQ(childDeps[0].key.type, DepType::EnvVar);
}

TEST_F(TraceStoreTest, ColdStore_Deterministic)
{
    // Deterministic recording: same deps + result -> same trace (BSàlC: content-addressed trace)
    auto db = makeDb();
    std::vector<Dep> deps = {makeContentDep(pools(), "/a.nix", "a")};
    CachedResult value = string_t{"result", {}};

    auto r1 = db.record(rootPath(), value, deps, true);
    auto r2 = db.record(rootPath(), value, deps, true);

    // Same deps + same parent -> same trace (content-addressed deduplication)
    EXPECT_EQ(r1.traceId, r2.traceId);
}

TEST_F(TraceStoreTest, ColdStore_AllValueTypes)
{
    auto db = makeDb();

    auto testRoundtrip = [&](const CachedResult & value, std::string_view name) {
        auto pathId = vpath({name});
        db.record(pathId, value, {}, false);

        auto result = db.verify(pathId, {}, state);
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
    std::vector<Dep> deps = {makeContentDep(pools(), "/shared.nix", "shared")};

    // Two root attributes with identical deps should share the same trace (BSàlC: trace sharing)
    auto r1 = db.record(vpath({"a"}), string_t{"val1", {}}, deps, false);
    auto r2 = db.record(vpath({"b"}), string_t{"val2", {}}, deps, false);

    // Both should share the same trace ID (content-addressed)
    EXPECT_EQ(r1.traceId, r2.traceId);

    auto deps1 = db.loadFullTrace(r1.traceId);
    EXPECT_EQ(deps1.size(), 1u);
}

TEST_F(TraceStoreTest, EmptyTrace_HasDbRow)
{
    // Attributes with zero deps must still get a trace row (BSàlC: empty trace is valid)
    auto db = makeDb();

    auto result = db.record(rootPath(), string_t{"val", {}}, {}, true);

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
    std::vector<Dep> deps = {makeEnvVarDep(pools(), "NIX_TEST_CACHE_VAR", "expected_value")};
    auto result = db.record(rootPath(), null_t{}, deps, true);

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
    std::vector<Dep> deps = {makeEnvVarDep(pools(), "NIX_TEST_CACHE_VAR", "old_value")};
    auto result = db.record(rootPath(), null_t{}, deps, true);

    // Clear session memo cache so verifyTrace re-checks all deps (Salsa: force re-verification)
    db.clearSessionCaches();

    bool valid = db.verifyTrace(result.traceId, {}, state);
    EXPECT_FALSE(valid);
}

TEST_F(TraceStoreTest, VerifyTrace_CurrentTime_AlwaysFails)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeCurrentTimeDep(pools())};
    auto result = db.record(rootPath(), null_t{}, deps, true);

    // CurrentTime is volatile — verification always fails (Shake: always-dirty rule)
    bool valid = db.verifyTrace(result.traceId, {}, state);
    EXPECT_FALSE(valid);
}

TEST_F(TraceStoreTest, VerifyTrace_Exec_AlwaysFails)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeExecDep(pools())};
    auto result = db.record(rootPath(), null_t{}, deps, true);

    bool valid = db.verifyTrace(result.traceId, {}, state);
    EXPECT_FALSE(valid);
}

TEST_F(TraceStoreTest, VerifyTrace_SessionCacheHit)
{
    ScopedEnvVar env("NIX_TEST_CACHE_VAR2", "val");

    auto db = makeDb();
    std::vector<Dep> deps = {makeEnvVarDep(pools(), "NIX_TEST_CACHE_VAR2", "val")};
    auto result = db.record(rootPath(), null_t{}, deps, true);

    // Recording with non-volatile deps should session-memo the trace (Salsa: memoization)
    EXPECT_TRUE(db.verifiedTraceIds.count(result.traceId));
    // Second call hits session memo — skips re-verification (Salsa: green query)
    EXPECT_TRUE(db.verifyTrace(result.traceId, {}, state));
}

TEST_F(TraceStoreTest, VerifyTrace_NoDeps_Valid)
{
    auto db = makeDb();
    auto result = db.record(rootPath(), string_t{"val", {}}, {}, true);

    db.clearSessionCaches();

    bool valid = db.verifyTrace(result.traceId, {}, state);
    EXPECT_TRUE(valid);
}

TEST_F(TraceStoreTest, VerifyTrace_ParentInvalid_ChildSurvives)
{
    ScopedEnvVar env("NIX_TEST_PARENT", "current_value");

    auto db = makeDb();

    // Record parent trace with stale dep
    std::vector<Dep> staleDeps = {makeEnvVarDep(pools(), "NIX_TEST_PARENT", "stale_value")};
    db.record(rootPath(), string_t{"parent", {}}, staleDeps, true);

    // Record child with no direct deps — child has no deps to invalidate
    auto childResult = db.record(vpath({"child"}), string_t{"child", {}},
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
    std::vector<Dep> parentDeps = {makeEnvVarDep(pools(), "NIX_TEST_PARENT", "correct_value")};
    db.record(rootPath(), string_t{"parent", {}},
              parentDeps, true);

    // Record child with valid parent (Shake: transitive clean)
    auto childResult = db.record(vpath({"child"}), string_t{"child", {}},
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
        makeEnvVarDep(pools(), "NIX_TEST_VALID", "current_value"),  // dep hash matches current
        makeEnvVarDep(pools(), "NIX_TEST_STALE", "old_value"),      // dep hash stale (Shake: dirty input)
    };
    auto result = db.record(rootPath(), null_t{}, deps, true);

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
    std::vector<Dep> deps = {makeEnvVarDep(pools(), "NIX_WARM_TEST", "stable")};

    db.record(rootPath(), input, deps, true);

    // Verification should find and validate the recorded trace (BSàlC: verify trace)
    auto result = db.verify(rootPath(), {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(input, result->value, state.symbols);
}

TEST_F(TraceStoreTest, RecordVerify_Roundtrip_TraceId)
{
    ScopedEnvVar env("NIX_WARM_TEST2", "stable");

    auto db = makeDb();
    CachedResult input = int_t{NixInt{99}};
    std::vector<Dep> deps = {makeEnvVarDep(pools(), "NIX_WARM_TEST2", "stable")};

    auto coldResult = db.record(rootPath(), input, deps, true);

    auto result = db.verify(rootPath(), {}, state);
    ASSERT_TRUE(result.has_value());
    // Verified trace's ID should match what recording returned
    EXPECT_EQ(result->traceId, coldResult.traceId);
}

TEST_F(TraceStoreTest, WarmPath_NoEntry)
{
    auto db = makeDb();
    auto result = db.verify(vpath({"nonexistent"}), {}, state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(TraceStoreTest, WarmPath_InvalidatedDeps)
{
    // Record trace with one env var value, then change it.
    // Verification should fail (no constructive recovery candidate exists).
    ScopedEnvVar env("NIX_WARM_INVALID", "value1");

    auto db = makeDb();
    std::vector<Dep> deps = {makeEnvVarDep(pools(), "NIX_WARM_INVALID", "value1")};
    db.record(rootPath(), string_t{"old", {}}, deps, true);

    // Change env var — invalidates the recorded dep hash
    setenv("NIX_WARM_INVALID", "value2", 1);

    // Clear session memo cache to force re-verification (Salsa: invalidate memo)
    db.clearSessionCaches();

    auto result = db.verify(rootPath(), {}, state);
    // Should fail: trace hash no longer matches and no constructive recovery target exists
    EXPECT_FALSE(result.has_value());
}

} // namespace nix::eval_trace
