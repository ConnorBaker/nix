/**
 * Single-scope baseline tests for shape dep recording.
 * These should pass both before and after the materialization fix,
 * because they operate within a single DependencyTracker scope
 * where ExprTracedData::eval() creates proper PosIdx before
 * materialization occurs.
 */
#include "eval-trace/helpers.hh"

#include <format>
#include <gtest/gtest.h>

namespace nix::eval_trace::test {

// ── AttrNames tests ────────────────────────────────────────────────

TEST_F(DepPrecisionTest, SingleScope_AttrNames_JSON_RecordsSCKeys)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format("builtins.attrNames ({})", fj(f.path));
    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "SC #keys should be recorded for attrNames\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::ImplicitShape, "#keys"))
        << "IS #keys should be recorded at creation time\n" << dumpDeps(deps);
    // attrNames does NOT trigger #type recording
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#type"))
        << "SC #type should NOT be recorded for attrNames\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, SingleScope_HasAttr_JSON_Exists_RecordsSCHasKey)
{
    TempJsonFile f(R"({"x":1,"y":2})");
    auto expr = std::format("({}) ? x", fj(f.path));
    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#has:x"))
        << "SC #has:x should be recorded\n" << dumpDeps(deps);
    // hasAttr does NOT record #keys
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "SC #keys should NOT be recorded for hasAttr\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, SingleScope_HasAttr_JSON_Missing_RecordsSCHasKey)
{
    TempJsonFile f(R"({"x":1,"y":2})");
    auto expr = std::format("({}) ? z", fj(f.path));
    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#has:z"))
        << "SC #has:z (missing) should be recorded\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, SingleScope_TypeOf_JSON_RecordsSCType)
{
    TempJsonFile f(R"({"a":1})");
    auto expr = std::format("builtins.typeOf ({})", fj(f.path));
    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#type"))
        << "SC #type should be recorded for typeOf\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, SingleScope_MapAttrs_AttrNames_RecordsSCKeys)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format("builtins.attrNames (builtins.mapAttrs (n: v: v + 1) ({}))", fj(f.path));
    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "SC #keys should be preserved through mapAttrs\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, SingleScope_AttrNames_TOML_RecordsSCKeys)
{
    TempTomlFile f("[section]\na = 1\nb = 2\n");
    auto expr = std::format("builtins.attrNames ({})", ft(f.path));
    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "SC #keys should be recorded for TOML attrNames\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, SingleScope_AttrNames_ReadDir_RecordsSCKeys)
{
    TempDir td;
    td.addFile("fileA");
    td.addFile("fileB");
    auto expr = std::format("builtins.attrNames ({})", rd(td.path()));
    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "SC #keys should be recorded for readDir attrNames\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, SingleScope_AttrNames_EmptyObject_RecordsSCKeys)
{
    TempJsonFile f(R"({})");
    auto expr = std::format("builtins.attrNames ({})", fj(f.path));
    auto deps = evalAndCollectDeps(expr);
    // Empty objects record SC #keys at creation time (no ImplicitShape)
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "SC #keys should be recorded for empty JSON object\n" << dumpDeps(deps);
}

} // namespace nix::eval_trace::test
