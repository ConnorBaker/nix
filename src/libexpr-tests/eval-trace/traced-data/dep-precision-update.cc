#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/dependency-tracker.hh"
#include "nix/expr/eval-trace-deps.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

/**
 * Dep-precision tests for the // (update) operator.
 *
 * These tests verify EXACTLY which dependencies are recorded to ensure
 * we are not under- or over-approximating. The // operator should NOT
 * record #keys deps — only consumer sites (attrNames, hasAttr, .x access)
 * should record the deps they genuinely need.
 *
 * Positive tests: verify that consumer-site deps ARE recorded.
 * Negative tests: verify that // does NOT record #keys deps.
 */

// ═══════════════════════════════════════════════════════════════════════
// Helper: evaluate expr with DependencyTracker and return collected deps
// ═══════════════════════════════════════════════════════════════════════

class DepPrecisionUpdateTest : public TracedDataTest
{
protected:
    std::vector<Dep> evalAndCollectDeps(const std::string & nixExpr)
    {
        DependencyTracker tracker;
        auto v = eval(nixExpr, /* forceValue */ true);
        return tracker.collectTraces();
    }

    /// Count deps of a given type whose key contains a substring.
    static size_t countDeps(const std::vector<Dep> & deps, DepType type, const std::string & keySubstr)
    {
        size_t n = 0;
        for (auto & d : deps)
            if (d.type == type && d.key.find(keySubstr) != std::string::npos)
                n++;
        return n;
    }

    /// Check if any dep of a given type has a key containing a substring.
    static bool hasDep(const std::vector<Dep> & deps, DepType type, const std::string & keySubstr)
    {
        return countDeps(deps, type, keySubstr) > 0;
    }
};

// ═══════════════════════════════════════════════════════════════════════
// Negative: // does NOT record #keys (StructuredContent) deps
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionUpdateTest, Update_NoSCKeysRecorded_SingleProvenance)
{
    // (traced // nix).a — the // should NOT record StructuredContent #keys.
    // Only ImplicitShape #keys (from creation time) and scalar dep (from .a) expected.
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = R"((builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"() // { c = 3; }).a)";

    auto deps = evalAndCollectDeps(expr);

    // Positive: scalar dep for .a should exist
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "a"))
        << "Expected StructuredContent scalar dep for accessed key 'a'";

    // Positive: ImplicitShape #keys should exist (from creation time)
    EXPECT_TRUE(hasDep(deps, DepType::ImplicitShape, "#keys"))
        << "Expected ImplicitShape #keys from ExprTracedData creation";

    // Negative: NO StructuredContent #keys should exist (// must not record it)
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "// must NOT record StructuredContent #keys — only consumer sites should";
}

TEST_F(DepPrecisionUpdateTest, Update_NoSCKeysRecorded_MultiProvenance)
{
    // (traced_a // traced_b).x — // must not record StructuredContent #keys
    // for either operand.
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"z": 3})");
    auto expr = R"((builtins.fromJSON (builtins.readFile )" + fileA.path.string()
        + R"() // builtins.fromJSON (builtins.readFile )" + fileB.path.string()
        + R"()).x)";

    auto deps = evalAndCollectDeps(expr);

    // Positive: scalar dep for .x should exist (from a's origin)
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "x"))
        << "Expected StructuredContent scalar dep for accessed key 'x'";

    // Positive: ImplicitShape #keys for both origins
    EXPECT_GE(countDeps(deps, DepType::ImplicitShape, "#keys"), 2u)
        << "Expected ImplicitShape #keys from both traced data sources";

    // Negative: NO StructuredContent #keys
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "// must NOT record StructuredContent #keys for either operand";
}

TEST_F(DepPrecisionUpdateTest, Update_NoSCKeysRecorded_Chained)
{
    // (a // b // c).x — chained // must not record StructuredContent #keys
    TempJsonFile fileA(R"({"x": 1})");
    TempJsonFile fileB(R"({"y": 2})");
    TempJsonFile fileC(R"({"z": 3})");
    auto expr = R"((builtins.fromJSON (builtins.readFile )" + fileA.path.string()
        + R"() // builtins.fromJSON (builtins.readFile )" + fileB.path.string()
        + R"() // builtins.fromJSON (builtins.readFile )" + fileC.path.string()
        + R"()).x)";

    auto deps = evalAndCollectDeps(expr);

    // Positive: scalar dep for .x
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "x"));

    // Negative: NO StructuredContent #keys from any // operation
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Chained // must NOT record StructuredContent #keys";
}

// ═══════════════════════════════════════════════════════════════════════
// Positive: consumer sites DO record the correct deps
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionUpdateTest, Update_AttrNames_RecordsSCKeys)
{
    // builtins.attrNames (traced // nix) — attrNames SHOULD record
    // StructuredContent #keys (the consumer genuinely observes the key set).
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = R"(builtins.attrNames (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"() // { c = 3; }))";

    auto deps = evalAndCollectDeps(expr);

    // Positive: attrNames records StructuredContent #keys
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "attrNames must record StructuredContent #keys — it observes the key set";
}

TEST_F(DepPrecisionUpdateTest, Update_HasAttr_RecordsHasKey)
{
    // builtins.hasAttr "x" (traced // nix) — hasAttr SHOULD record #has:x
    TempJsonFile file(R"({"x": 1, "b": 2})");
    auto expr = R"(builtins.hasAttr "x" (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"() // { c = 3; }))";

    auto deps = evalAndCollectDeps(expr);

    // Positive: hasAttr records #has:x
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#has:x"))
        << "hasAttr must record StructuredContent #has:x";

    // Negative: no StructuredContent #keys from //
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "hasAttr + // must not produce StructuredContent #keys";
}

TEST_F(DepPrecisionUpdateTest, Update_ScalarAccess_RecordsScalarDep)
{
    // (traced // nix).a — scalar access SHOULD record the value dep
    TempJsonFile file(R"({"a": "hello", "b": 2})");
    auto expr = R"((builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"() // { c = 3; }).a)";

    auto deps = evalAndCollectDeps(expr);

    // Positive: scalar dep for .a
    size_t scalarDeps = 0;
    for (auto & d : deps) {
        if (d.type == DepType::StructuredContent
            && d.key.find("#keys") == std::string::npos
            && d.key.find("#has:") == std::string::npos
            && d.key.find("#len") == std::string::npos
            && d.key.find("#type") == std::string::npos
            && d.key.find("a") != std::string::npos)
            scalarDeps++;
    }
    EXPECT_GE(scalarDeps, 1u)
        << "Scalar access .a must record a StructuredContent value dep";
}

// ═══════════════════════════════════════════════════════════════════════
// Cache behavior: precision improvement from removing #keys at //
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionUpdateTest, Update_KeyAdded_ScalarAccess_CacheHit)
{
    // (traced // nix).a where traced gains an unrelated key — cache hit.
    // This is the precision improvement: without #keys from //, adding
    // an unrelated key does not invalidate.
    TempJsonFile file(R"({"a": "x", "b": "y"})");
    auto expr = R"((builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"() // { extra = "e"; }).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x"));
    }

    file.modify(R"({"a": "x", "b": "y", "c": "z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0) << "Adding unrelated key 'c' must not invalidate .a access";
        EXPECT_THAT(v, IsStringEq("x"));
    }
}

TEST_F(DepPrecisionUpdateTest, Update_KeyAdded_AttrNames_CacheMiss)
{
    // builtins.attrNames (traced // nix) where traced gains a key — cache miss.
    // attrNames records #keys which catches the key addition.
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = R"(builtins.attrNames (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"() // { c = 3; }))";

    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    file.modify(R"({"a": 1, "b": 2, "d": 4})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1) << "attrNames must detect key addition via #keys dep";
    }
}

TEST_F(DepPrecisionUpdateTest, Update_MultiProv_ImplicitShapeFallback_CacheHit)
{
    // (a // b).x where x from a; change value in b (not keys) — cache hit.
    // ImplicitShape #keys fallback verifies b's keys are unchanged.
    TempJsonFile fileA(R"({"x": 1})");
    TempJsonFile fileB(R"({"y": 2})");
    auto expr = R"((builtins.fromJSON (builtins.readFile )" + fileA.path.string()
        + R"() // builtins.fromJSON (builtins.readFile )" + fileB.path.string()
        + R"()).x)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    fileB.modify(R"({"y": 99})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0) << "ImplicitShape fallback: b's keys unchanged → cache hit";
        EXPECT_THAT(v, IsIntEq(1));
    }
}

TEST_F(DepPrecisionUpdateTest, Update_MultiProv_ImplicitShapeFallback_KeyChange_CacheMiss)
{
    // (a // b).x where x from a; b gains key "x" — cache miss.
    // ImplicitShape #keys fallback detects b's key set changed.
    TempJsonFile fileA(R"({"x": 1})");
    TempJsonFile fileB(R"({"y": 2})");
    auto expr = R"((builtins.fromJSON (builtins.readFile )" + fileA.path.string()
        + R"() // builtins.fromJSON (builtins.readFile )" + fileB.path.string()
        + R"()).x)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    // b gains key "x" — now b's x would win in the merge
    fileB.modify(R"({"y": 2, "x": 99})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1) << "ImplicitShape fallback: b's keys changed → cache miss";
        EXPECT_THAT(v, IsIntEq(99)); // b's x wins now
    }
}

} // namespace nix::eval_trace
