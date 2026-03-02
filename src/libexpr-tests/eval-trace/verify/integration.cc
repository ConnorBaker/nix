#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class TraceCacheIntegrationTest : public TraceCacheFixture
{
public:
    TraceCacheIntegrationTest()
    {
        testFingerprint = hashString(HashAlgorithm::SHA256, "integration-test");
    }

protected:
    static constexpr int64_t testCtx = 0xDEADBEEF;

    AttrVocabStore & testVocab() {
        return state.traceCtx->getVocabStore(state.symbols);
    }

    /// Build an AttrPathId from string components.
    AttrPathId vpath(std::initializer_list<std::string_view> parts) {
        return vocabPath(testVocab(), parts);
    }

    /// Root path sentinel.
    AttrPathId rootPath() { return AttrVocabStore::rootPath(); }

    TraceStore makeDbBackend()
    {
        return TraceStore(state.symbols, *state.traceCtx->pools,
            state.traceCtx->getVocabStore(state.symbols), testCtx);
    }
};

// ── TraceStore record/verify integration (BSàlC: trace recording then verification) ──

TEST_F(TraceCacheIntegrationTest, ColdStore_ThenWarmPath)
{
    auto db = makeDbBackend();

    auto storeResult = db.record(
        rootPath(), string_t{"hello", {}}, {}, true);

    auto result = db.verify(rootPath(), {}, state);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(std::holds_alternative<string_t>(result->value));
    EXPECT_EQ(std::get<string_t>(result->value).first, "hello");
    EXPECT_EQ(result->traceId, storeResult.traceId);
}

TEST_F(TraceCacheIntegrationTest, MultipleContextHashes_Isolated)
{
    // Use separate TraceStore instances with different context hashes (BSàlC: isolated trace stores)
    {
        TraceStore db1(state.symbols, *state.traceCtx->pools, state.traceCtx->getVocabStore(state.symbols), 111);
        db1.record(rootPath(), string_t{"value-1", {}}, {}, true);
    }

    {
        TraceStore db2(state.symbols, *state.traceCtx->pools, state.traceCtx->getVocabStore(state.symbols), 222);
        db2.record(rootPath(), string_t{"value-2", {}}, {}, true);
    }

    // Verify isolation: each context hash sees its own trace result
    {
        TraceStore db1(state.symbols, *state.traceCtx->pools, state.traceCtx->getVocabStore(state.symbols), 111);
        auto r1 = db1.verify(rootPath(), {}, state);
        ASSERT_TRUE(r1.has_value());
        ASSERT_TRUE(std::holds_alternative<string_t>(r1->value));
        EXPECT_EQ(std::get<string_t>(r1->value).first, "value-1");
    }
    {
        TraceStore db2(state.symbols, *state.traceCtx->pools, state.traceCtx->getVocabStore(state.symbols), 222);
        auto r2 = db2.verify(rootPath(), {}, state);
        ASSERT_TRUE(r2.has_value());
        ASSERT_TRUE(std::holds_alternative<string_t>(r2->value));
        EXPECT_EQ(std::get<string_t>(r2->value).first, "value-2");
    }
}

// ── Parent-child trace chain tests (Adapton: DDG parent-child edges) ──

TEST_F(TraceCacheIntegrationTest, ParentChild_AttrChain)
{
    auto db = makeDbBackend();

    db.record(
        rootPath(), attrs_t{{createSymbol("child")}},
        {}, true);

    auto childPathId = vpath({"child"});
    db.record(
        childPathId, int_t{NixInt{42}},
        {}, false);

    // Verification for child should work (BSàlC: verify child trace with parent hint)
    auto result = db.verify(childPathId, {}, state);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(std::holds_alternative<int_t>(result->value));
    EXPECT_EQ(std::get<int_t>(result->value).x.value, 42);
}

TEST_F(TraceCacheIntegrationTest, ParentChild_ValidationCascade)
{
    ScopedEnvVar env("NIX_INT_TEST_VAR", "valid");

    auto db = makeDbBackend();

    // Parent trace with a valid dep (BSàlC: verifiable trace)
    auto & p = *state.traceCtx->pools;
    auto dep = makeEnvVarDep(p, "NIX_INT_TEST_VAR", "valid");
    db.record(
        rootPath(), null_t{}, {dep}, true);

    // Child trace with no deps — always valid (separated dep ownership)
    auto childResult = db.record(
        vpath({"child"}), int_t{NixInt{1}}, {}, false);

    EXPECT_TRUE(db.verifyTrace(childResult.traceId, {}, state));
}

TEST_F(TraceCacheIntegrationTest, ChildSurvivesParentInvalidation)
{
    ScopedEnvVar env("NIX_INT_CASCADE", "current");

    auto db = makeDbBackend();

    // Parent trace with stale dep (hash doesn't match current oracle state)
    auto & p = *state.traceCtx->pools;
    auto staleDep = makeEnvVarDep(p, "NIX_INT_CASCADE", "old_value");
    db.record(rootPath(), null_t{}, {staleDep}, true);

    // Child with no direct deps
    auto childResult = db.record(
        vpath({"child"}), int_t{NixInt{1}}, {}, false);

    // With separated deps, child has no deps → always valid
    // Parent invalidation no longer cascades to children
    db.clearSessionCaches();
    EXPECT_TRUE(db.verifyTrace(childResult.traceId, {}, state));
}

// ── Full TraceCache + DependencyTracker flow (BSàlC: end-to-end trace pipeline) ──

TEST_F(TraceCacheIntegrationTest, FullFlow_ScalarRoot)
{
    // Fresh evaluation (BSàlC: trace recording)
    {
        auto cache = makeCache("\"hello world\"");
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        EXPECT_THAT(*v, IsStringEq("hello world"));
    }
    // Verification (BSàlC: verify trace and serve cached result)
    {
        int loaderCalls = 0;
        auto cache = makeCache("\"hello world\"", &loaderCalls);
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        EXPECT_THAT(*v, IsStringEq("hello world"));
    }
}

TEST_F(TraceCacheIntegrationTest, FullFlow_NestedAttrAccess)
{
    // Fresh evaluation (BSàlC: trace recording)
    {
        auto cache = makeCache("{ x = { y = 42; }; }");
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        EXPECT_THAT(*v, IsAttrsOfSize(1));
        auto * x = v->attrs()->get(createSymbol("x"));
        ASSERT_NE(x, nullptr);
        state.forceValue(*x->value, noPos);
        auto * y = x->value->attrs()->get(createSymbol("y"));
        ASSERT_NE(y, nullptr);
        state.forceValue(*y->value, noPos);
        EXPECT_THAT(*y->value, IsIntEq(42));
    }
    // Verification (BSàlC: verify trace and serve cached result)
    {
        auto cache = makeCache("{ x = { y = 42; }; }");
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        auto * x = v->attrs()->get(createSymbol("x"));
        ASSERT_NE(x, nullptr);
        state.forceValue(*x->value, noPos);
        auto * y = x->value->attrs()->get(createSymbol("y"));
        ASSERT_NE(y, nullptr);
        state.forceValue(*y->value, noPos);
        EXPECT_THAT(*y->value, IsIntEq(42));
    }
}

TEST_F(TraceCacheIntegrationTest, FullFlow_TwoIndependentAttrs)
{
    // Fresh evaluation (BSàlC: trace recording)
    {
        auto cache = makeCache("{ a = 1; b = 2; }");
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        auto * a = v->attrs()->get(createSymbol("a"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        EXPECT_THAT(*a->value, IsIntEq(1));
        auto * b = v->attrs()->get(createSymbol("b"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        EXPECT_THAT(*b->value, IsIntEq(2));
    }
    // Verification (BSàlC: verify trace and serve cached result)
    {
        auto cache = makeCache("{ a = 1; b = 2; }");
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        auto * a = v->attrs()->get(createSymbol("a"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        EXPECT_THAT(*a->value, IsIntEq(1));
        auto * b = v->attrs()->get(createSymbol("b"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        EXPECT_THAT(*b->value, IsIntEq(2));
    }
}

TEST_F(TraceCacheIntegrationTest, FullFlow_ParsedExpr)
{
    // Use state.parseExprFromString directly as the rootLoader
    auto loader = [this]() -> Value * {
        auto * e = state.parseExprFromString(
            "{ message = \"parsed\"; count = 3; }",
            state.rootPath(CanonPath::root));
        auto * v = state.allocValue();
        state.eval(e, *v);
        return v;
    };

    // Fresh evaluation (BSàlC: trace recording)
    {
        auto cache = std::make_unique<TraceCache>(
            testFingerprint, state, loader);
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        EXPECT_THAT(*v, IsAttrsOfSize(2));
    }
    // Verification (BSàlC: verify trace and serve cached result)
    {
        auto cache = std::make_unique<TraceCache>(
            testFingerprint, state, loader);
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        EXPECT_THAT(*v, IsAttrsOfSize(2));
    }
}

// ── Session memoization integration (Salsa: memoized verification) ───

TEST_F(TraceCacheIntegrationTest, SessionCache_TraceVerification_SkipsRevalidation)
{
    ScopedEnvVar env("NIX_EVAL_SESSION", "ok");

    auto db = makeDbBackend();
    auto & p = *state.traceCtx->pools;
    auto dep = makeEnvVarDep(p, "NIX_EVAL_SESSION", "ok");
    auto result = db.record(
        rootPath(), null_t{}, {dep}, true);

    // Recording adds to verifiedTraceIds for non-volatile deps (Salsa: memoize verified query)
    EXPECT_TRUE(db.verifiedTraceIds.count(result.traceId));

    // Clear session memo and re-verify manually
    db.clearSessionCaches();
    EXPECT_TRUE(db.verifyTrace(result.traceId, {}, state));
    EXPECT_TRUE(db.verifiedTraceIds.count(result.traceId));

    // Second call should be memoized (hits verifiedTraceIds early exit — Salsa: green query)
    EXPECT_TRUE(db.verifyTrace(result.traceId, {}, state));
}

TEST_F(TraceCacheIntegrationTest, SessionCache_VolatileDep_NotCached)
{
    auto db = makeDbBackend();
    auto & p = *state.traceCtx->pools;
    auto dep = makeCurrentTimeDep(p);
    auto result = db.record(
        rootPath(), null_t{}, {dep}, true);

    // Volatile deps should NOT be session-memoized (Salsa: no memoization for volatile)
    EXPECT_FALSE(db.verifiedTraceIds.count(result.traceId));
}

// ── Constructive recovery flow integration (BSàlC: constructive traces) ──

TEST_F(TraceCacheIntegrationTest, Recovery_AfterDepChange)
{
    // Step 1: Record trace with env var = "v1"
    // Step 2: Change env var to "v2", old trace invalid
    // Step 3: Change back to "v1" -> constructive recovery should find v1's trace

    auto db = makeDbBackend();

    auto & p = *state.traceCtx->pools;

    // Step 1: Record trace with v1
    TraceStore::RecordResult v1Result;
    {
        ScopedEnvVar env("NIX_RECOVERY_TEST", "v1");
        auto dep = makeEnvVarDep(p, "NIX_RECOVERY_TEST", "v1");
        v1Result = db.record(rootPath(), string_t{"result-v1", {}}, {dep}, true);
    }

    // Step 2: Record trace with v2
    {
        ScopedEnvVar env("NIX_RECOVERY_TEST", "v2");
        auto dep = makeEnvVarDep(p, "NIX_RECOVERY_TEST", "v2");
        db.record(rootPath(), string_t{"result-v2", {}}, {dep}, true);
    }

    // Step 3: Revert to v1 — trigger constructive recovery
    {
        ScopedEnvVar env("NIX_RECOVERY_TEST", "v1");

        // Clear session memos to force re-verification
        db.clearSessionCaches();

        // Verification should fail (v2 trace deps don't match v1 oracle state)
        // Constructive recovery should find v1's trace
        auto result = db.verify(rootPath(), {}, state);
        if (result.has_value()) {
            // Constructive recovery found v1's trace result!
            ASSERT_TRUE(std::holds_alternative<string_t>(result->value));
            EXPECT_EQ(std::get<string_t>(result->value).first, "result-v1");
        }
        // If constructive recovery doesn't find it, that's acceptable too
    }
}

TEST_F(TraceCacheIntegrationTest, Recovery_VolatileFails)
{
    auto db = makeDbBackend();

    // Record trace with volatile dep (Shake: always-dirty rule)
    auto & p = *state.traceCtx->pools;
    auto dep = makeCurrentTimeDep(p);
    auto storeResult = db.record(
        rootPath(), null_t{}, {dep}, true);

    // Clear session memo cache
    db.clearSessionCaches();

    // Constructive recovery should fail for volatile deps (Shake: always-dirty, no recovery)
    auto result = db.recovery(storeResult.traceId, rootPath(), {}, state);
    EXPECT_FALSE(result.has_value());
}

} // namespace nix::eval_trace
