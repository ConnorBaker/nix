/**
 * Dep-precision tests for Nix language features interacting with traced data.
 *
 * Exercises: with statement, inherit (expr), rec attrsets, string interpolation,
 * arithmetic, comparison, logical operations.
 * Each test verifies exact dep types (positive + negative).
 */
#include "eval-trace/helpers.hh"

#include <format>
#include <gtest/gtest.h>

namespace nix::eval_trace::test {

// ── with statement on traced data ───────────────────────────────────

TEST_F(DepPrecisionTest, With_FieldAccess_RecordsContent)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    // `with d; a + b` accesses fields from the `with` scope
    auto expr = std::format(
        "let d = {}; in with d; a + b", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    // Accessing fields through `with` still reads the file
    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "with field access should record Content dep\n" << dumpDeps(deps);
    // `with` doesn't call attrNames/hasAttr/typeOf → no shape deps
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
        << "with field access should NOT record SC #keys\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, With_DirectAttrNames_RecordsSCKeys)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = {}; in with d; builtins.attrNames d", fj(f.path));
    // attrNames on the original traced data (not a derived attrset)
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
        << "with + direct attrNames on traced data should record SC #keys\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, With_HasAttr_RecordsSCHasKey)
{
    TempJsonFile f(R"({"x":1,"y":2})");
    auto expr = std::format(
        "let d = {}; in with d; d ? x", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("x")))
        << "with + hasAttr on traced data should record SC #has:x\n" << dumpDeps(deps);
}

// ── inherit (expr) pattern ──────────────────────────────────────────

TEST_F(DepPrecisionTest, InheritFrom_NoSCKeys)
{
    TempJsonFile f(R"({"a":1,"b":2,"c":3})");
    auto expr = std::format(
        "let d = {}; in builtins.attrNames {{ inherit (d) a b; }}", fj(f.path));
    // inherit (d) a b creates a Nix attrset with a, b extracted from d.
    // The result is a Nix construction — no TracedData PosIdx.
    auto deps = evalAndCollectDeps(expr);

    // The new attrset has Nix-defined attrs, not TracedData
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
        << "inherit (expr) creates Nix attrset — no SC #keys on result\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, InheritFrom_RecordsContent)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = {}; in ({{ inherit (d) a; }}).a", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "inherit (expr) should still record Content dep for the source file\n" << dumpDeps(deps);
}

// ── rec attrsets with traced data ───────────────────────────────────

TEST_F(DepPrecisionTest, RecAttrset_TracedData_RecordsContent)
{
    TempJsonFile f(R"({"x":10})");
    auto expr = std::format(
        "let d = {}; in (rec {{ val = d.x; doubled = val * 2; }}).doubled", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "rec attrset accessing traced data should record Content dep\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, RecAttrset_AttrNamesOnTraced_RecordsSCKeys)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = {}; in (rec {{ keys = builtins.attrNames d; data = d; }}).keys", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
        << "rec attrset with attrNames on traced data should record SC #keys\n" << dumpDeps(deps);
}

// ── String interpolation with traced values ─────────────────────────

TEST_F(DepPrecisionTest, StringInterp_TracedField_RecordsContent)
{
    TempJsonFile f(R"({"name":"hello"})");
    auto expr = std::format(
        "let d = {}; in \"prefix-${{d.name}}-suffix\"", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "String interpolation of traced field should record Content dep\n" << dumpDeps(deps);
    // String interpolation doesn't call attrNames/hasAttr/typeOf
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
        << "String interpolation should NOT record SC #keys\n" << dumpDeps(deps);
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("type")))
        << "String interpolation should NOT record SC #type\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, StringInterp_ToString_RecordsContent)
{
    TempJsonFile f(R"({"val":42})");
    auto expr = std::format(
        "let d = {}; in \"value=${{builtins.toString d.val}}\"", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "toString in interpolation should record Content dep\n" << dumpDeps(deps);
}

// ── Arithmetic operations on traced values ──────────────────────────

TEST_F(DepPrecisionTest, Arithmetic_Add_RecordsContent)
{
    TempJsonFile f(R"({"x":10,"y":20})");
    auto expr = std::format(
        "let d = {}; in d.x + d.y", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "Arithmetic on traced values should record Content dep\n" << dumpDeps(deps);
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
        << "Arithmetic should NOT record SC #keys\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, Arithmetic_Mul_RecordsContent)
{
    TempJsonFile f(R"({"x":5})");
    auto expr = std::format(
        "let d = {}; in d.x * 3", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "Multiplication of traced value should record Content dep\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, Arithmetic_Negate_RecordsContent)
{
    TempJsonFile f(R"({"x":5})");
    auto expr = std::format(
        "let d = {}; in 0 - d.x", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "Negation of traced value should record Content dep\n" << dumpDeps(deps);
}

// ── Comparison operations on traced values ──────────────────────────

TEST_F(DepPrecisionTest, Comparison_LessThan_RecordsContent)
{
    TempJsonFile f(R"({"x":5})");
    auto expr = std::format(
        "let d = {}; in d.x < 10", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "< comparison on traced value should record Content dep\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, Comparison_Equality_RecordsContent)
{
    TempJsonFile f(R"({"x":5})");
    auto expr = std::format(
        "let d = {}; in d.x == 5", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "== comparison on traced value should record Content dep\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, Comparison_Inequality_RecordsContent)
{
    TempJsonFile f(R"({"x":5})");
    auto expr = std::format(
        "let d = {}; in d.x != 10", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "!= comparison on traced value should record Content dep\n" << dumpDeps(deps);
}

// ── Logical operations on traced values ─────────────────────────────

TEST_F(DepPrecisionTest, Logical_And_RecordsContent)
{
    TempJsonFile f(R"({"a":true,"b":false})");
    auto expr = std::format(
        "let d = {}; in d.a && d.b", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "&& on traced booleans should record Content dep\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, Logical_Or_RecordsContent)
{
    TempJsonFile f(R"({"a":true,"b":false})");
    auto expr = std::format(
        "let d = {}; in d.a || d.b", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "|| on traced booleans should record Content dep\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, Logical_Not_RecordsContent)
{
    TempJsonFile f(R"({"flag":true})");
    auto expr = std::format(
        "let d = {}; in !d.flag", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "! on traced boolean should record Content dep\n" << dumpDeps(deps);
}

// ── If-then-else with traced condition ──────────────────────────────

TEST_F(DepPrecisionTest, IfThenElse_TracedCondition_RecordsContent)
{
    TempJsonFile f(R"({"enabled":true,"val":42})");
    auto expr = std::format(
        "let d = {}; in if d.enabled then d.val else 0", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "if-then-else with traced condition should record Content dep\n" << dumpDeps(deps);
}

// ── List concatenation with traced data ─────────────────────────────

TEST_F(DepPrecisionTest, ListConcat_TracedLists_RecordsContent)
{
    TempJsonFile f1(R"([1,2])");
    TempJsonFile f2(R"([3,4])");
    auto expr = std::format(
        "({}) ++ ({})", fj(f1.path), fj(f2.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::Content, f1.path.filename().string()))
        << "List concat should record Content for file 1\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::Content, f2.path.filename().string()))
        << "List concat should record Content for file 2\n" << dumpDeps(deps);
}

// ── builtins.seq forcing traced data ────────────────────────────────

TEST_F(DepPrecisionTest, Seq_ForcesTracedData_RecordsContent)
{
    TempJsonFile f(R"({"x":1})");
    auto expr = std::format(
        "builtins.seq ({}) 42", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "builtins.seq forcing traced data should record Content dep\n" << dumpDeps(deps);
}

// ── builtins.deepSeq forcing traced data ────────────────────────────

TEST_F(DepPrecisionTest, DeepSeq_ForcesTracedData_RecordsContent)
{
    TempJsonFile f(R"({"x":1,"y":{"z":2}})");
    auto expr = std::format(
        "builtins.deepSeq ({}) 42", fj(f.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "builtins.deepSeq should record Content dep\n" << dumpDeps(deps);
}

} // namespace nix::eval_trace::test
