/**
 * Dep-precision tests for function application over traced data.
 *
 * Exercises: lambda consuming traced data, map with field access predicate,
 * fold accumulating traced data, formal pattern matching on traced data.
 * Each test verifies exact dep types (positive + negative).
 */
#include "eval-trace/helpers.hh"

#include <format>
#include <gtest/gtest.h>

namespace nix::eval_trace::test {

// ── Lambda consuming traced data ────────────────────────────────────

TEST_F(DepPrecisionTest, Lambda_AttrNames_RecordsSCKeys)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let f = x: builtins.attrNames x; in f ({})", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Lambda consuming traced data should record SC #keys\n" << dumpDeps(deps);
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#type"))
        << "attrNames through lambda should NOT record SC #type\n" << dumpDeps(deps);
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#has:"))
        << "attrNames through lambda should NOT record SC #has\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, Lambda_HasAttr_RecordsSCHasKey)
{
    TempJsonFile f(R"({"x":1,"y":2})");
    auto expr = std::format(
        "let check = d: d ? x; in check ({})", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#has:x"))
        << "Lambda hasAttr should record SC #has:x\n" << dumpDeps(deps);
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Lambda hasAttr should NOT record SC #keys\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, Lambda_TypeOf_RecordsSCType)
{
    TempJsonFile f(R"({"a":1})");
    auto expr = std::format(
        "let getType = x: builtins.typeOf x; in getType ({})", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#type"))
        << "Lambda typeOf should record SC #type\n" << dumpDeps(deps);
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Lambda typeOf should NOT record SC #keys\n" << dumpDeps(deps);
}

// ── Map with field access predicate ─────────────────────────────────

TEST_F(DepPrecisionTest, Map_FieldAccess_RecordsContentDep)
{
    TempJsonFile f(R"([{"v":10},{"v":20},{"v":30}])");
    auto expr = std::format(
        "builtins.map (x: x.v) ({})", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "Map over traced list should record Content dep\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::ImplicitShape, "#len"))
        << "Traced array should have ImplicitShape #len\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, Map_AttrNames_RecordsSCKeys)
{
    TempJsonFile f(R"({"a":1,"b":2,"c":3})");
    auto expr = std::format(
        "builtins.map (x: x) (builtins.attrNames ({}))", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Map over attrNames should record SC #keys\n" << dumpDeps(deps);
}

// ── Fold accumulating traced data ───────────────────────────────────

TEST_F(DepPrecisionTest, Foldl_AccumulateKeys_RecordsSCKeys)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "builtins.foldl' (acc: k: acc + k) \"\" (builtins.attrNames ({}))",
        fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "foldl' over attrNames should record SC #keys\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, Foldl_MergeAttrs_RecordsContent)
{
    TempJsonFile f1(R"({"a":1})");
    TempJsonFile f2(R"({"b":2})");
    auto expr = std::format(
        "builtins.foldl' (acc: x: acc // x) {{}} [({}) ({})]",
        fj(f1.path), fj(f2.path));
    auto deps = evalAndCollectDeps(expr);

    // Both files should have Content deps
    EXPECT_TRUE(hasDep(deps, DepType::Content, f1.path.filename().string()))
        << "foldl' merge should record Content for file 1\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::Content, f2.path.filename().string()))
        << "foldl' merge should record Content for file 2\n" << dumpDeps(deps);
}

// ── Formal pattern matching on traced data ──────────────────────────

TEST_F(DepPrecisionTest, StrictFormals_RecordsSCKeys)
{
    TempJsonFile f(R"({"x":1,"y":2})");
    auto expr = std::format(
        "let apply = {{ x, y }}: x + y; in apply ({})", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    // Strict formals use forceAttrs + direct attr access, NOT the ? operator.
    // They record SC #keys (from the strict check) and per-field SC value deps.
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Strict formals should record SC #keys\n" << dumpDeps(deps);
    // Individual field values are accessed → SC value deps
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "j:x"))
        << "Strict formals accessing x should record SC value dep\n" << dumpDeps(deps);
    // Strict formals do NOT use ? operator → no #has:key deps
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#has:"))
        << "Strict formals should NOT record SC #has:key\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, StrictFormals_NoSCType)
{
    TempJsonFile f(R"({"x":1,"y":2})");
    auto expr = std::format(
        "let apply = {{ x, y }}: x + y; in apply ({})", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#type"))
        << "Strict formals should NOT record SC #type\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, FormalDefault_RecordsSCKeysAndValue)
{
    TempJsonFile f(R"({"x":1})");
    auto expr = std::format(
        "let apply = {{ x, y ? 0 }}: x + y; in apply ({})", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    // Formal matching records SC #keys (the strict key check).
    // x is accessed → SC value dep for x.
    // y is missing, default used → no SC value dep for y.
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Formal with default should record SC #keys\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "j:x"))
        << "Present formal field should record SC value dep\n" << dumpDeps(deps);
    // Formal matching does NOT use ? operator → no #has: deps
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#has:"))
        << "Formal matching should NOT record SC #has:key\n" << dumpDeps(deps);
}

// ── Nested lambda + traced data ─────────────────────────────────────

TEST_F(DepPrecisionTest, NestedLambda_AttrNames_RecordsSCKeys)
{
    TempJsonFile f(R"({"inner":{"a":1,"b":2}})");
    auto expr = std::format(
        "let getInnerKeys = d: builtins.attrNames d.inner; in getInnerKeys ({})",
        fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Nested lambda attrNames should record SC #keys for inner\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, HigherOrder_MapAttrNames_RecordsSCKeys)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let apply = f: d: f d; in apply builtins.attrNames ({})", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Higher-order function should record SC #keys\n" << dumpDeps(deps);
}

} // namespace nix::eval_trace::test
