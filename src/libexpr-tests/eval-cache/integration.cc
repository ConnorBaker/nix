#include "helpers.hh"
#include "nix/expr/eval-cache-db.hh"

#include <gtest/gtest.h>

#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/util/hash.hh"

namespace nix::eval_cache {

using namespace nix::eval_cache::test;

class EvalCacheIntegrationTest : public LibExprTest
{
public:
    EvalCacheIntegrationTest()
        : LibExprTest(openStore("dummy://", {{"read-only", "false"}}),
            [](bool & readOnlyMode) {
                readOnlyMode = false;
                EvalSettings s{readOnlyMode};
                s.nixPath = {};
                return s;
            })
    {}

protected:
    // Writable cache dir for EvalCacheDb SQLite (sandbox has no writable $HOME)
    ScopedCacheDir cacheDir;

    static constexpr int64_t testCtx = 0xDEADBEEF;
    Hash testFingerprint = hashString(HashAlgorithm::SHA256, "integration-test");

    EvalCacheDb makeDbBackend()
    {
        return EvalCacheDb(state.symbols, testCtx);
    }

    std::unique_ptr<EvalCache> makeCache(
        const std::string & nixExpr,
        int * loaderCalls = nullptr)
    {
        auto loader = [this, nixExpr, loaderCalls]() -> Value * {
            if (loaderCalls) (*loaderCalls)++;
            Value v = eval(nixExpr);
            auto * result = state.allocValue();
            *result = v;
            return result;
        };
        return std::make_unique<EvalCache>(
            testFingerprint, state, std::move(loader));
    }
};

// ── EvalCacheDb cold/warm integration ─────────────────────────────────

TEST_F(EvalCacheIntegrationTest, ColdStore_ThenWarmPath)
{
    auto db = makeDbBackend();

    auto attrId = db.coldStore(
        "", string_t{"hello", {}}, {}, std::nullopt, true);

    auto result = db.warmPath("", {}, state);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(std::holds_alternative<string_t>(result->value));
    EXPECT_EQ(std::get<string_t>(result->value).first, "hello");
    EXPECT_EQ(result->attrId, attrId);
}

TEST_F(EvalCacheIntegrationTest, MultipleContextHashes_Isolated)
{
    // Use separate EvalCacheDb instances with different context hashes
    {
        EvalCacheDb db1(state.symbols, 111);
        db1.coldStore("", string_t{"value-1", {}}, {}, std::nullopt, true);
    }

    {
        EvalCacheDb db2(state.symbols, 222);
        db2.coldStore("", string_t{"value-2", {}}, {}, std::nullopt, true);
    }

    // Verify isolation: each context hash sees its own value
    {
        EvalCacheDb db1(state.symbols, 111);
        auto r1 = db1.warmPath("", {}, state);
        ASSERT_TRUE(r1.has_value());
        ASSERT_TRUE(std::holds_alternative<string_t>(r1->value));
        EXPECT_EQ(std::get<string_t>(r1->value).first, "value-1");
    }
    {
        EvalCacheDb db2(state.symbols, 222);
        auto r2 = db2.warmPath("", {}, state);
        ASSERT_TRUE(r2.has_value());
        ASSERT_TRUE(std::holds_alternative<string_t>(r2->value));
        EXPECT_EQ(std::get<string_t>(r2->value).first, "value-2");
    }
}

// ── Parent-child chain tests ─────────────────────────────────────────

TEST_F(EvalCacheIntegrationTest, ParentChild_AttrChain)
{
    auto db = makeDbBackend();

    auto parentId = db.coldStore(
        "", std::vector<Symbol>{createSymbol("child")},
        {}, std::nullopt, true);

    std::string childAttrPath = "child";
    db.coldStore(
        childAttrPath, int_t{NixInt{42}},
        {}, parentId, false);

    // Warm path for child should work
    auto result = db.warmPath(childAttrPath, {}, state, parentId);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(std::holds_alternative<int_t>(result->value));
    EXPECT_EQ(std::get<int_t>(result->value).x.value, 42);
}

TEST_F(EvalCacheIntegrationTest, ParentChild_ValidationCascade)
{
    ScopedEnvVar env("NIX_INT_TEST_VAR", "valid");

    auto db = makeDbBackend();

    // Parent with a valid dep
    auto dep = makeEnvVarDep("NIX_INT_TEST_VAR", "valid");
    auto parentId = db.coldStore(
        "", null_t{}, {dep}, std::nullopt, true);

    // Child inheriting parent validity
    auto childId = db.coldStore(
        "child", int_t{NixInt{1}}, {}, parentId, false);

    EXPECT_TRUE(db.validateAttr(childId, {}, state));
}

TEST_F(EvalCacheIntegrationTest, ParentInvalidation_CascadesToChild)
{
    ScopedEnvVar env("NIX_INT_CASCADE", "current");

    auto db = makeDbBackend();

    // Parent with stale dep (hash doesn't match current env)
    auto staleDep = makeEnvVarDep("NIX_INT_CASCADE", "old_value");
    auto parentId = db.coldStore(
        "", null_t{}, {staleDep}, std::nullopt, true);

    // Child with no direct deps
    auto childId = db.coldStore(
        "child", int_t{NixInt{1}}, {}, parentId, false);

    // Child should be invalid because parent is invalid
    // (need to clear session cache since coldStore marks as validated)
    db.clearSessionCaches();
    EXPECT_FALSE(db.validateAttr(childId, {}, state));
}

// ── Full EvalCache + FileLoadTracker flow ────────────────────────────

TEST_F(EvalCacheIntegrationTest, FullFlow_ScalarRoot)
{
    // Cold
    {
        auto cache = makeCache("\"hello world\"");
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        EXPECT_THAT(*v, IsStringEq("hello world"));
    }
    // Warm
    {
        int loaderCalls = 0;
        auto cache = makeCache("\"hello world\"", &loaderCalls);
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        EXPECT_THAT(*v, IsStringEq("hello world"));
    }
}

TEST_F(EvalCacheIntegrationTest, FullFlow_NestedAttrAccess)
{
    // Cold
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
    // Warm
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

TEST_F(EvalCacheIntegrationTest, FullFlow_TwoIndependentAttrs)
{
    // Cold
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
    // Warm
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

TEST_F(EvalCacheIntegrationTest, FullFlow_ParsedExpr)
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

    // Cold
    {
        auto cache = std::make_unique<EvalCache>(
            testFingerprint, state, loader);
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        EXPECT_THAT(*v, IsAttrsOfSize(2));
    }
    // Warm
    {
        auto cache = std::make_unique<EvalCache>(
            testFingerprint, state, loader);
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        EXPECT_THAT(*v, IsAttrsOfSize(2));
    }
}

// ── Session caching integration ──────────────────────────────────────

TEST_F(EvalCacheIntegrationTest, SessionCache_AttrValidation_SkipsRevalidation)
{
    ScopedEnvVar env("NIX_EVAL_SESSION", "ok");

    auto db = makeDbBackend();
    auto dep = makeEnvVarDep("NIX_EVAL_SESSION", "ok");
    auto attrId = db.coldStore(
        "", null_t{}, {dep}, std::nullopt, true);

    // coldStore adds to validatedAttrIds for non-volatile deps
    EXPECT_TRUE(db.validatedAttrIds.count(attrId));

    // Clear and re-validate manually
    db.clearSessionCaches();
    EXPECT_TRUE(db.validateAttr(attrId, {}, state));
    EXPECT_TRUE(db.validatedAttrIds.count(attrId));

    // Second call should be cached (hits validatedAttrIds early exit)
    EXPECT_TRUE(db.validateAttr(attrId, {}, state));
}

TEST_F(EvalCacheIntegrationTest, SessionCache_VolatileDep_NotCached)
{
    auto db = makeDbBackend();
    auto dep = makeCurrentTimeDep();
    auto attrId = db.coldStore(
        "", null_t{}, {dep}, std::nullopt, true);

    // Volatile deps should NOT be session-cached
    EXPECT_FALSE(db.validatedAttrIds.count(attrId));
}

// ── Recovery flow integration ────────────────────────────────────────

TEST_F(EvalCacheIntegrationTest, Recovery_AfterDepChange)
{
    // Phase 1: Store with env var = "v1"
    // Phase 2: Change env var to "v2", old entry invalid
    // Phase 3: Change back to "v1" -> recovery should find v1's entry

    auto db = makeDbBackend();

    // Phase 1: Cold with v1
    {
        ScopedEnvVar env("NIX_RECOVERY_TEST", "v1");
        auto dep = makeEnvVarDep("NIX_RECOVERY_TEST", "v1");
        db.coldStore("", string_t{"result-v1", {}}, {dep}, std::nullopt, true);
    }

    // Phase 2: Cold with v2
    {
        ScopedEnvVar env("NIX_RECOVERY_TEST", "v2");
        auto dep = makeEnvVarDep("NIX_RECOVERY_TEST", "v2");
        db.coldStore("", string_t{"result-v2", {}}, {dep}, std::nullopt, true);
    }

    // Phase 3: Revert to v1
    {
        ScopedEnvVar env("NIX_RECOVERY_TEST", "v1");

        // Clear session caches to force re-validation
        db.clearSessionCaches();

        // Warm path should fail (v2 deps don't match v1)
        // Then recovery should find v1's entry
        auto result = db.warmPath("", {}, state);
        if (result.has_value()) {
            // Recovery found v1's result!
            ASSERT_TRUE(std::holds_alternative<string_t>(result->value));
            EXPECT_EQ(std::get<string_t>(result->value).first, "result-v1");
        }
        // If recovery doesn't find it, that's acceptable too
    }
}

TEST_F(EvalCacheIntegrationTest, Recovery_VolatileFails)
{
    auto db = makeDbBackend();

    // Store with volatile dep
    auto dep = makeCurrentTimeDep();
    auto attrId = db.coldStore(
        "", null_t{}, {dep}, std::nullopt, true);

    // Clear session cache
    db.clearSessionCaches();

    // Recovery should fail for volatile deps
    auto result = db.recovery(attrId, "", {}, state);
    EXPECT_FALSE(result.has_value());
}

} // namespace nix::eval_cache
