/**
 * Cross-scope materialization tests for attrNames / SC #keys.
 * These tests exercise the cross-TracedExpr boundary where
 * materialization occurs: a root attrset with a TracedData child
 * and a sibling that observes the child's shape.
 *
 * Before the fix: these tests FAIL (no SC shape deps on materialized values).
 * After the fix: all tests PASS.
 */
#include "eval-trace/helpers.hh"

#include <format>
#include <gtest/gtest.h>

namespace nix::eval_trace::test {

// ── Basic cross-scope #keys tests ──────────────────────────────────

TEST_F(MaterializationDepTest, CrossScope_AttrNames_JSON_RecordsSCKeys)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    auto deps = getStoredDeps("names");
    EXPECT_TRUE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
        << "Cross-scope: SC #keys should be recorded for attrNames on materialized value\n"
        << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, CrossScope_AttrNames_TOML)
{
    TempTomlFile f("[section]\na = 1\nb = 2\n");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d; }}",
        ft(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    auto deps = getStoredDeps("names");
    EXPECT_TRUE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
        << "Cross-scope TOML: SC #keys should be recorded\n" << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, CrossScope_AttrNames_ReadDir)
{
    TempDir td;
    td.addFile("fileA");
    td.addFile("fileB");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d; }}",
        rd(td.path()));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    auto deps = getStoredDeps("names");
    EXPECT_TRUE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
        << "Cross-scope readDir: SC #keys should be recorded\n" << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, CrossScope_AttrNames_MapAttrs_PreservesProvenance)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = builtins.mapAttrs (n: v: v + 1) ({}); in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    auto deps = getStoredDeps("names");
    EXPECT_TRUE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
        << "Cross-scope mapAttrs: SC #keys should be preserved through mapAttrs\n"
        << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, CrossScope_AttrNames_Nested_RecordsSCKeys)
{
    TempJsonFile f(R"({"inner":{"x":1,"y":2}})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d.inner; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    auto deps = getStoredDeps("names");
    EXPECT_TRUE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
        << "Cross-scope nested: SC #keys should be recorded for d.inner attrNames\n"
        << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, CrossScope_AttrValues_RecordsSCKeys)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; vals = builtins.attrValues d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * valsAttr = root.attrs()->get(state.symbols.create("vals"));
        ASSERT_NE(valsAttr, nullptr);
        state.forceValue(*valsAttr->value, noPos);
    }

    auto deps = getStoredDeps("vals");
    EXPECT_TRUE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
        << "Cross-scope attrValues: SC #keys should be recorded\n"
        << dumpDeps(deps);
}

// ── Negative tests ─────────────────────────────────────────────────

TEST_F(MaterializationDepTest, CrossScope_AttrNames_NoSCType)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    auto deps = getStoredDeps("names");
    EXPECT_FALSE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("type")))
        << "Cross-scope: attrNames should NOT record SC #type\n" << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, CrossScope_AttrNames_NoSCHasKey)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    auto deps = getStoredDeps("names");
    EXPECT_FALSE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, [](const nlohmann::json & j) { return j.contains("h"); }))
        << "Cross-scope: attrNames should NOT record SC #has\n" << dumpDeps(deps);
}

// ── Multi-origin tests ─────────────────────────────────────────────

TEST_F(MaterializationDepTest, CrossScope_AttrNames_MultiOrigin_RecordsSCKeysPerOrigin)
{
    TempJsonFile f1(R"({"a":1})");
    TempJsonFile f2(R"({"b":2})");
    auto expr = std::format(
        "let d = ({}) // ({}); in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f1.path), fj(f2.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    auto deps = getStoredDeps("names");
    auto scKeysCount = countJsonDeps(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys"));
    EXPECT_GE(scKeysCount, 2u)
        << "Multi-origin: should have SC #keys for each origin\n" << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, CrossScope_AttrNames_MixedOrigin_RecordsSCKeysForTracedOnly)
{
    TempJsonFile f(R"({"a":1})");
    auto expr = std::format(
        "let d = ({}) // {{ extra = 1; }}; in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    auto deps = getStoredDeps("names");
    EXPECT_TRUE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
        << "Mixed origin: SC #keys should be recorded for the TracedData origin\n"
        << dumpDeps(deps);
}

// ── Cache invalidation tests ───────────────────────────────────────

TEST_F(MaterializationDepTest, CrossScope_AttrNames_KeyAdded_CacheMiss)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f.path));

    // First evaluation — populates cache
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    // Change JSON: add a key
    f.modify(R"({"a":1,"b":2,"c":3})");
    invalidateFileCache(f.path);

    // Second evaluation — should get fresh deps; force list elements
    // so child TracedExpr thunks record their traces
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
        for (size_t i = 0; i < namesAttr->value->listSize(); i++)
            state.forceValue(*namesAttr->value->listView()[i], noPos);
        EXPECT_EQ(loaderCalls, 1) << "key addition → cache miss";
    }

    // The list is stored as list_t (child elements are separate traces)
    auto result = getStoredResult("names");
    ASSERT_TRUE(result.has_value());
    auto * lt = std::get_if<list_t>(& *result);
    ASSERT_NE(lt, nullptr);
    ASSERT_EQ(lt->entries.size(), 3u) << "After key addition, names should have 3 elements";

    // Verify stored child element values
    std::vector<std::string> expected{"a", "b", "c"};
    for (size_t i = 0; i < expected.size(); i++) {
        auto childResult = getStoredResult("names." + std::to_string(i));
        ASSERT_TRUE(childResult.has_value()) << "Child " << i << " should have a stored trace";
        auto * s = std::get_if<string_t>(& *childResult);
        ASSERT_NE(s, nullptr) << "Child " << i << " should be a string";
        EXPECT_EQ(s->first, expected[i]) << "Child " << i << " should be " << expected[i];
    }
}

// Symmetry gap (§N.3): the structurally-equivalent
// `NestedValueChange_AttrNames_CacheHit` in nested-invalidation.cc
// (which uses `d.inner` instead of top-level `d`) produces a warm hit
// (loaderCalls == 0) after a same-keys value change. THIS test, with
// top-level `d`, produces loaderCalls == 1 — the cross-scope
// materialization path at the top level doesn't pick up the #keys SC
// hit the same way the nested path does. The assertion pins the
// current behavior so a future fix (or regression) is visible.
// TODO(DEF-3): investigate the top-level vs nested asymmetry in the
// cross-scope attrNames materialization. See
// src/libexpr-tests/eval-trace/CLAUDE.md §D (Deferred Work Index).
TEST_F(MaterializationDepTest, CrossScope_AttrNames_ValueChanged_TopLevelCacheHit)
{
    // Symmetric counterpart to NestedValueChange_AttrNames_CacheHit in
    // nested-invalidation.cc: top-level `d` vs nested `d.inner`.  Both
    // shapes must produce a warm hit when a JSON value changes but the
    // key set stays the same — the #keys SC dep covers precision.
    //
    // Historical note: until 2026-04-22 this was pinned as a "symmetry
    // gap" with `EXPECT_EQ(loaderCalls, 1)` because the WARM block here
    // iterated and forced every list element returned by `attrNames d`.
    // Those list-element children (`names.0`, `names.1`) had no
    // stored-trace row — the COLD block above never forced them, so
    // `materializeResult`'s `list_t` branch created TracedExpr child
    // thunks that were never published.  Warm `verifyAttrImpl` then
    // returned `nullopt` for each, `TracedExpr::eval` fell through to
    // `evaluateFresh`, and the rootLoader fired.  This has nothing to
    // do with the top-level/nested asymmetry that was hypothesised —
    // see DEF-3 in src/libexpr-tests/eval-trace/CLAUDE.md.
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f.path));

    // First evaluation
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    // Change a value but not the keys
    f.modify(R"({"a":99,"b":99})");
    invalidateFileCache(f.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
        EXPECT_EQ(loaderCalls, 0)
            << "top-level attrNames with unchanged key set → warm hit";
    }

    auto result = getStoredResult("names");
    ASSERT_TRUE(result.has_value());
    auto * lt = std::get_if<list_t>(& *result);
    ASSERT_NE(lt, nullptr);
    EXPECT_EQ(lt->entries.size(), 2u) << "Names should still have 2 elements after value change";
}

} // namespace nix::eval_trace::test
