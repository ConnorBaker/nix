#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/dependency-tracker.hh"
#include "nix/expr/eval-trace-deps.hh"

#include <format>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

/**
 * Comprehensive dep-precision tests for lambda formals.
 *
 * Sections:
 *   A. Strict formals — positive dep recording
 *   B. Strict formals — negative (should NOT record)
 *   C. Ellipsis formals — positive (what SHOULD be recorded)
 *   D. Ellipsis formals — attrNames interaction
 *   E. Single arg (no formals) — baseline
 *   F. Multi-source (// merge) with formals
 *   G. Nested function application
 *   H. TOML source with formals
 */

class FormalsComprehensiveTest : public DepPrecisionTest {};

// ═══════════════════════════════════════════════════════════════════════
// A. Strict formals — positive dep recording
// ═══════════════════════════════════════════════════════════════════════

TEST_F(FormalsComprehensiveTest, StrictFormals_TwoFields_RecordsSCKeysAndValues)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({{ a, b }}: a + b) ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Strict formals must record SC #keys\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "j:a"))
        << "Field a accessed in body\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "j:b"))
        << "Field b accessed in body\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "Must have Content dep for file read\n" << dumpDeps(deps);
}

TEST_F(FormalsComprehensiveTest, StrictFormals_PartialAccess_RecordsKeysAndAccessedField)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({{ a, b }}: a) ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Strict formals must record SC #keys\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "j:a"))
        << "Field a accessed in body\n" << dumpDeps(deps);
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "j:b"))
        << "Field b not accessed in body — no SC dep\n" << dumpDeps(deps);
}

TEST_F(FormalsComprehensiveTest, StrictFormals_WithDefault_PresentField_RecordsValue)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({{ a, b ? 0 }}: a + b) ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Strict formals must record SC #keys\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "j:a"))
        << "Field a accessed in body\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "j:b"))
        << "Field b present and accessed in body\n" << dumpDeps(deps);
}

TEST_F(FormalsComprehensiveTest, StrictFormals_WithDefault_MissingField_NoValueDep)
{
    TempJsonFile file(R"({"a": 1})");
    auto expr = std::format("({{ a, b ? 0 }}: a + b) ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Strict formals must record SC #keys\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "j:a"))
        << "Field a accessed in body\n" << dumpDeps(deps);
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "j:b"))
        << "Field b missing (default used) — no SC value dep\n" << dumpDeps(deps);
}

TEST_F(FormalsComprehensiveTest, StrictFormals_Named_RecordsSCKeys)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("(args@{{ a, b }}: a + args.b) ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Named strict formals must record SC #keys\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "j:a"))
        << "Field a accessed via formal\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "j:b"))
        << "Field b accessed via args.b\n" << dumpDeps(deps);
}

TEST_F(FormalsComprehensiveTest, StrictFormals_EmptyObject_RecordsSCKeys)
{
    TempJsonFile file(R"({})");
    auto expr = std::format("({{}}: 42) ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Empty strict formals must record SC #keys (empty key set hash)\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// B. Strict formals — negative (should NOT record)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(FormalsComprehensiveTest, StrictFormals_NoSCType)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({{ a, b }}: a + b) ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#type"))
        << "Formals should not record #type\n" << dumpDeps(deps);
}

TEST_F(FormalsComprehensiveTest, StrictFormals_NoSCHasKey)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({{ a, b }}: a + b) ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#has:"))
        << "Formals use get(), not ? operator — no #has: deps\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// C. Ellipsis formals — positive (what SHOULD be recorded)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(FormalsComprehensiveTest, EllipsisFormals_SingleField_RecordsValueOnly)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({{ a, ... }}: a) ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "j:a"))
        << "Field a accessed in body\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "Must have Content dep for file read\n" << dumpDeps(deps);
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Ellipsis formals must NOT record #keys\n" << dumpDeps(deps);
}

TEST_F(FormalsComprehensiveTest, EllipsisFormals_TwoFields_RecordsValuesOnly)
{
    TempJsonFile file(R"({"a": 1, "b": 2, "c": 3})");
    auto expr = std::format("({{ a, b, ... }}: a + b) ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "j:a"))
        << "Field a accessed in body\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "j:b"))
        << "Field b accessed in body\n" << dumpDeps(deps);
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Ellipsis formals must NOT record #keys\n" << dumpDeps(deps);
}

TEST_F(FormalsComprehensiveTest, EllipsisFormals_WithDefault_Present_RecordsValue)
{
    TempJsonFile file(R"({"a": 1, "b": 2, "c": 3})");
    auto expr = std::format("({{ a, b ? 0, ... }}: a + b) ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "j:a"))
        << "Field a accessed in body\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "j:b"))
        << "Field b present and accessed in body\n" << dumpDeps(deps);
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Ellipsis formals must NOT record #keys\n" << dumpDeps(deps);
}

TEST_F(FormalsComprehensiveTest, EllipsisFormals_WithDefault_Missing_NoValueDep)
{
    TempJsonFile file(R"({"a": 1, "c": 3})");
    auto expr = std::format("({{ a, b ? 0, ... }}: a + b) ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "j:a"))
        << "Field a accessed in body\n" << dumpDeps(deps);
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Ellipsis formals must NOT record #keys\n" << dumpDeps(deps);
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "j:b"))
        << "Field b missing (default used) — no SC value dep\n" << dumpDeps(deps);
}

TEST_F(FormalsComprehensiveTest, EllipsisFormals_Named_NoSCKeys)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("(args@{{ a, ... }}: a) ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "j:a"))
        << "Field a accessed in body\n" << dumpDeps(deps);
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Named ellipsis formals must NOT record #keys\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// D. Ellipsis formals — attrNames interaction
// ═══════════════════════════════════════════════════════════════════════

TEST_F(FormalsComprehensiveTest, EllipsisFormals_AttrNamesOnArg_RecordsSCKeysViaBuiltin)
{
    // builtins.attrNames records SC #keys — NOT formals
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("(args@{{ a, ... }}: builtins.attrNames args) ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "attrNames records SC #keys even with ellipsis formals\n" << dumpDeps(deps);
}

TEST_F(FormalsComprehensiveTest, EllipsisFormals_AttrNamesOnArg_PlusFieldAccess)
{
    // Both #keys (from attrNames) and j:a (from field access) are recorded.
    // Use builtins.length to force attrNames, and add a to force field access.
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format(
        "(args@{{ a, ... }}: (builtins.length (builtins.attrNames args)) + a) ({})",
        fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "#keys from attrNames, not from formals\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "j:a"))
        << "j:a from field access\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// E. Single arg (no formals) — baseline
// ═══════════════════════════════════════════════════════════════════════

TEST_F(FormalsComprehensiveTest, SingleArg_FieldAccess_NoSCKeys)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("(x: x.a + x.b) ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "j:a"))
        << "Field a accessed via x.a\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "j:b"))
        << "Field b accessed via x.b\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "Must have Content dep for file read\n" << dumpDeps(deps);
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "No formals matching — no #keys\n" << dumpDeps(deps);
}

TEST_F(FormalsComprehensiveTest, SingleArg_AttrNames_RecordsSCKeysViaBuiltin)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("(x: builtins.attrNames x) ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "attrNames records SC #keys\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// F. Multi-source (// merge) with formals
// ═══════════════════════════════════════════════════════════════════════

TEST_F(FormalsComprehensiveTest, StrictFormals_Merged_RecordsSCKeysPerOrigin)
{
    TempJsonFile f1(R"({"a": 1})");
    TempJsonFile f2(R"({"b": 2})");
    auto expr = std::format(
        "({{ a, b }}: a + b) (({}) // ({}))", fj(f1.path), fj(f2.path));

    auto deps = evalAndCollectDeps(expr);

    // Strict formals on merged TracedData records #keys per origin
    auto keysCount = countDeps(deps, DepType::StructuredContent, "#keys");
    EXPECT_GE(keysCount, 2u)
        << "Strict formals on merged data should record #keys per origin\n" << dumpDeps(deps);
}

TEST_F(FormalsComprehensiveTest, EllipsisFormals_Merged_NoSCKeys)
{
    TempJsonFile f1(R"({"a": 1})");
    TempJsonFile f2(R"({"b": 2})");
    auto expr = std::format(
        "({{ a, ... }}: a) (({}) // ({}))", fj(f1.path), fj(f2.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "j:a"))
        << "Field a accessed in body\n" << dumpDeps(deps);
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Ellipsis on merged TracedData: no #keys from formals\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// G. Nested function application
// ═══════════════════════════════════════════════════════════════════════

TEST_F(FormalsComprehensiveTest, NestedStrict_OuterStrictInnerEllipsis)
{
    // f has ellipsis (no #keys), g has strict formals on Nix-constructed data
    // (not TracedData). Only f's application touches TracedData.
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format(
        "let f = {{ a, ... }}: a; g = {{ x, y }}: x; in g {{ x = f ({}); y = 0; }}",
        fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "j:a"))
        << "f's application: field a accessed\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "Must have Content dep for file read\n" << dumpDeps(deps);
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Neither formals call records #keys on TracedData\n" << dumpDeps(deps);
}

TEST_F(FormalsComprehensiveTest, NestedEllipsis_PassThrough)
{
    // wrap has ellipsis, inner is a TracedData sub-object
    TempJsonFile file(R"({"inner": {"a": 42}})");
    auto expr = std::format(
        "let wrap = {{ inner, ... }}: inner.a; in wrap ({})",
        fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "Must have Content dep for file read\n" << dumpDeps(deps);
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Ellipsis formals: no #keys\n" << dumpDeps(deps);
}

TEST_F(FormalsComprehensiveTest, StrictFormals_ResultPassedToAttrNames)
{
    // f has strict formals → records SC #keys on TracedData input.
    // builtins.attrNames on f's result (a Nix attrset, not TracedData)
    // doesn't add another SC #keys.
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format(
        "let f = {{ a, b }}: {{ inherit a b; }}; in builtins.attrNames (f ({}))",
        fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Strict formals on TracedData input records SC #keys\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// H. TOML source with formals
// ═══════════════════════════════════════════════════════════════════════

TEST_F(FormalsComprehensiveTest, StrictFormals_TOML_RecordsSCKeys)
{
    TempTomlFile file("a = 1\nb = 2");
    auto expr = std::format("({{ a, b }}: a + b) ({})", ft(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Strict formals on TOML data must record SC #keys\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "t:a"))
        << "TOML field a accessed in body\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "t:b"))
        << "TOML field b accessed in body\n" << dumpDeps(deps);
}

TEST_F(FormalsComprehensiveTest, EllipsisFormals_TOML_NoSCKeys)
{
    TempTomlFile file("a = 1\nb = 2");
    auto expr = std::format("({{ a, ... }}: a) ({})", ft(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "t:a"))
        << "TOML field a accessed in body\n" << dumpDeps(deps);
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Ellipsis formals on TOML data must NOT record #keys\n" << dumpDeps(deps);
}

} // namespace nix::eval_trace
