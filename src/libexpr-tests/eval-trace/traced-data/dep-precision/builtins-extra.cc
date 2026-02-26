#include "eval-trace/helpers.hh"

#include <format>
#include <gtest/gtest.h>

#include "nix/expr/dependency-tracker.hh"
#include "nix/expr/eval-trace-deps.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DepPrecisionBuiltinsExtraTest : public DepPrecisionTest {};

// ═══════════════════════════════════════════════════════════════════════
// Builtins that should not affect dep tracking
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionBuiltinsExtraTest, Trace_DoesNotAffectDeps)
{
    // builtins.trace should not introduce or suppress any deps
    TempJsonFile file(R"({"x": 42})");
    auto exprWithTrace = std::format(R"(builtins.trace "msg" ({}).x)", fj(file.path));
    auto exprWithout = std::format("({}).x", fj(file.path));

    auto depsWith = evalAndCollectDeps(exprWithTrace);
    auto depsWithout = evalAndCollectDeps(exprWithout);

    // Both should have SC dep for x
    EXPECT_TRUE(hasDep(depsWith, DepType::StructuredContent, "x"))
        << "trace must not suppress SC deps\n" << dumpDeps(depsWith);
    EXPECT_TRUE(hasDep(depsWithout, DepType::StructuredContent, "x"));

    // Both should have IS #keys
    EXPECT_TRUE(hasDep(depsWith, DepType::ImplicitShape, "#keys"));
    EXPECT_TRUE(hasDep(depsWithout, DepType::ImplicitShape, "#keys"));
}

TEST_F(DepPrecisionBuiltinsExtraTest, Seq_ForcesOne_RecordsDeps)
{
    // builtins.seq forces first arg then returns second
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = std::format("let j = {}; in builtins.seq j.x j.y", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    // Should record SC deps for both x and y (both are forced)
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "x"))
        << "seq forces first arg\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "y"))
        << "seq returns second arg (which is also forced)\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionBuiltinsExtraTest, DeepSeq_ForcesAll_RecordsDeps)
{
    // deepSeq forces the entire first arg deeply
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = std::format("let j = {}; in builtins.deepSeq j j.x", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    // deepSeq forces all values but doesn't call shape-observing builtins
    // (attrNames, length) — so no SC #keys or #len. It does record scalar
    // deps for the forced leaf values and j.x access.
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, ":x"))
        << "deepSeq + j.x records scalar dep for x\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::ImplicitShape, "#keys"))
        << "IS #keys from creation\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionBuiltinsExtraTest, TryEval_RecordsDeps)
{
    // tryEval wraps evaluation in a try/catch but should preserve deps
    TempJsonFile file(R"({"x": 42})");
    auto expr = std::format("(builtins.tryEval ({}).x).value", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "x"))
        << "tryEval must preserve SC deps\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionBuiltinsExtraTest, TryEval_CacheBehavior)
{
    TempJsonFile file(R"({"x": 42})");
    auto expr = std::format("(builtins.tryEval ({}).x).value", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(42));
    }

    // Change value -- must invalidate
    file.modify(R"({"x": 99})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(99));
    }
}

TEST_F(DepPrecisionBuiltinsExtraTest, FunctionArgs_NoTracedDeps)
{
    // functionArgs introspects lambda, not traced data -- no SC deps
    TempJsonFile file(R"({"x": 1})");
    auto expr = R"(builtins.functionArgs ({ a, b ? 2 }: a))";

    auto deps = evalAndCollectDeps(expr);
    // No SC deps -- this doesn't touch traced data
    EXPECT_EQ(countDepsByType(deps, DepType::StructuredContent), 0u)
        << "functionArgs must not produce SC deps\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionBuiltinsExtraTest, ToString_AttrsetCoerce_CacheMiss)
{
    // toString { outPath = (fromJSON ...).path; } -- coerces via outPath
    TempJsonFile file(R"({"path": "/nix/store/abc"})");
    auto expr = std::format("toString {{ outPath = ({}).path; }}", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "path"))
            << "toString coercion must record SC dep for path\n" << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("/nix/store/abc"));
    }

    file.modify(R"({"path": "/nix/store/def"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("/nix/store/def"));
    }
}

TEST_F(DepPrecisionBuiltinsExtraTest, GenericClosure_RecordsDeps)
{
    // genericClosure iterates startSet -- should record #len
    TempJsonFile file(R"({"items": [{"key": 1, "next": []}, {"key": 2, "next": []}]})");
    auto expr = std::format(
        "let j = {}; in builtins.length (builtins.genericClosure {{ startSet = j.items; operator = item: []; }})",
        fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify(R"({"items": [{"key": 1, "next": []}, {"key": 2, "next": []}, {"key": 3, "next": []}]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(3));
    }
}

TEST_F(DepPrecisionBuiltinsExtraTest, AddErrorContext_DoesNotAffectDeps)
{
    TempJsonFile file(R"({"x": 42})");
    auto expr = std::format(R"(builtins.addErrorContext "ctx" ({}).x)", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "x"))
        << "addErrorContext must not affect dep recording\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionBuiltinsExtraTest, MapAttrsRecursive_RecordsDeps)
{
    // lib.mapAttrsRecursive doesn't exist as a builtin; use nested mapAttrs
    TempJsonFile file(R"({"a": {"b": 1}, "c": {"d": 2}})");
    auto expr = std::format(
        "let j = {}; in builtins.attrNames (builtins.mapAttrs (n: v: builtins.mapAttrs (n2: v2: v2) v) j)",
        fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Nested mapAttrs -> attrNames records SC #keys\n" << dumpDeps(deps);
}

} // namespace nix::eval_trace
