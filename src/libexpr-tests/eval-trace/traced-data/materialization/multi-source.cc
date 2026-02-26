/**
 * Cross-scope tests for multi-source TracedData interactions.
 *
 * Exercises: JSON+TOML merge, readDir→fromJSON chain, two JSON files merged.
 * Each test verifies exact dep types and cache invalidation behavior.
 */
#include "eval-trace/helpers.hh"

#include <format>
#include <gtest/gtest.h>

namespace nix::eval_trace::test {

// ── JSON + TOML merge ───────────────────────────────────────────────

TEST_F(MaterializationDepTest, JsonTomlMerge_AttrNames_RecordsSCKeysPerOrigin)
{
    TempJsonFile fj_file(R"({"a":1})");
    TempTomlFile ft_file("b = 2\n");
    auto expr = std::format(
        "let d = ({}) // ({}); in {{ inherit d; names = builtins.attrNames d; }}",
        fj(fj_file.path), ft(ft_file.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
    }

    auto deps = getStoredDeps("names");
    auto scKeysCount = countDeps(deps, DepType::StructuredContent, "#keys");
    EXPECT_GE(scKeysCount, 2u)
        << "JSON+TOML merge should record SC #keys for each origin\n" << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, JsonTomlMerge_JsonKeyAdded_CacheMiss)
{
    TempJsonFile fj_file(R"({"a":1})");
    TempTomlFile ft_file("b = 2\n");
    auto expr = std::format(
        "let d = ({}) // ({}); in {{ inherit d; names = builtins.attrNames d; }}",
        fj(fj_file.path), ft(ft_file.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        ASSERT_EQ(attr->value->listSize(), 2u); // a, b
    }

    fj_file.modify(R"({"a":1,"c":3})");
    invalidateFileCache(fj_file.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->listSize(), 3u)
            << "JSON key addition should invalidate merged attrNames (a, b, c)";
    }
}

TEST_F(MaterializationDepTest, JsonTomlMerge_TomlValueChanged_CacheHit)
{
    TempJsonFile fj_file(R"({"a":1})");
    TempTomlFile ft_file("b = 2\n");
    auto expr = std::format(
        "let d = ({}) // ({}); in {{ inherit d; names = builtins.attrNames d; }}",
        fj(fj_file.path), ft(ft_file.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        ASSERT_EQ(attr->value->listSize(), 2u);
    }

    ft_file.modify("b = 99\n");
    invalidateFileCache(ft_file.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->listSize(), 2u)
            << "TOML value change (not keys) should NOT invalidate attrNames";
    }
}

// ── readDir → fromJSON chain ────────────────────────────────────────

TEST_F(MaterializationDepTest, ReadDirThenJson_AttrNames_RecordsDeps)
{
    TempDir td;
    td.addFile("a.json", R"({"x":1})");
    td.addFile("b.json", R"({"y":2})");
    auto expr = std::format(
        "let listing = {}; in {{ inherit listing; names = builtins.attrNames listing; }}",
        rd(td.path()));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        ASSERT_EQ(attr->value->listSize(), 2u);
    }

    auto deps = getStoredDeps("names");
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "readDir attrNames should record SC #keys\n" << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, ReadDirThenJson_FileAdded_CacheMiss)
{
    TempDir td;
    td.addFile("a.json", R"({"x":1})");
    auto expr = std::format(
        "let listing = {}; in {{ inherit listing; names = builtins.attrNames listing; }}",
        rd(td.path()));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        ASSERT_EQ(attr->value->listSize(), 1u);
    }

    td.addFile("b.json", R"({"y":2})");
    INVALIDATE_DIR(td);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->listSize(), 2u)
            << "readDir: file addition should invalidate attrNames";
    }
}

// ── Two JSON files merged cross-scope ───────────────────────────────

TEST_F(MaterializationDepTest, TwoJsonMerge_IndependentInvalidation)
{
    TempJsonFile f1(R"({"a":1})");
    TempJsonFile f2(R"({"b":2})");
    auto expr = std::format(
        "let d = ({}) // ({}); in {{ inherit d; names = builtins.attrNames d; val = d.a; }}",
        fj(f1.path), fj(f2.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
        auto * valAttr = root.attrs()->get(state.symbols.create("val"));
        ASSERT_NE(valAttr, nullptr);
        state.forceValue(*valAttr->value, noPos);
    }

    // Change only f2 — val (from f1) should still be valid
    f2.modify(R"({"b":2,"c":3})");
    invalidateFileCache(f2.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
        EXPECT_EQ(namesAttr->value->listSize(), 3u)
            << "names should have 3 elements after f2 key addition (a, b, c)";

        auto * valAttr = root.attrs()->get(state.symbols.create("val"));
        ASSERT_NE(valAttr, nullptr);
        state.forceValue(*valAttr->value, noPos);
        EXPECT_EQ(valAttr->value->integer().value, 1)
            << "val from f1 should still be 1 (f1 unchanged)";
    }
}

// ── JSON + readDir merge ────────────────────────────────────────────

TEST_F(MaterializationDepTest, JsonReadDirMerge_RecordsSCKeysPerOrigin)
{
    TempJsonFile fj_file(R"({"fromJson":1})");
    TempDir td;
    td.addFile("fromDir");
    auto expr = std::format(
        "let d = ({}) // ({}); in {{ inherit d; names = builtins.attrNames d; }}",
        fj(fj_file.path), rd(td.path()));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        ASSERT_EQ(attr->value->listSize(), 2u);
    }

    auto deps = getStoredDeps("names");
    auto scKeysCount = countDeps(deps, DepType::StructuredContent, "#keys");
    EXPECT_GE(scKeysCount, 2u)
        << "JSON+readDir merge should record SC #keys for each origin\n" << dumpDeps(deps);
}

} // namespace nix::eval_trace::test
