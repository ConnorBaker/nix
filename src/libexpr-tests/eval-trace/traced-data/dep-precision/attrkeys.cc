#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/types.hh"

#include <format>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DepPrecisionAttrKeysTest : public DepPrecisionTest {};

// ═══════════════════════════════════════════════════════════════════════
// Dep verification: attrNames records SC #keys
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionAttrKeysTest, AttrNames_RecordsSCKeys)
{
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = std::format("builtins.attrNames ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
        << "attrNames must record SC #keys\n" << dumpDeps(deps);
    EXPECT_TRUE(hasJsonDep(deps, DepType::ImplicitShape, shapePred("keys")))
        << "Object creation must record IS #keys\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionAttrKeysTest, AttrNames_NoSCType)
{
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = std::format("builtins.attrNames ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("type")))
        << "attrNames must NOT record SC #type\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionAttrKeysTest, AttrNames_NoSCLen)
{
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = std::format("builtins.attrNames ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("len")))
        << "attrNames must NOT record SC #len\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionAttrKeysTest, AttrValues_RecordsSCKeys)
{
    TempJsonFile file(R"({"a": "x", "b": "y"})");
    auto expr = std::format("builtins.concatStringsSep \",\" (builtins.attrValues ({}))", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
        << "attrValues must record SC #keys\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionAttrKeysTest, AttrValues_NoSCType)
{
    TempJsonFile file(R"({"a": "x", "b": "y"})");
    auto expr = std::format("builtins.concatStringsSep \",\" (builtins.attrValues ({}))", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("type")))
        << "attrValues must NOT record SC #type\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// Cache behavior: attrNames key set changes (from shape-core.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionAttrKeysTest, AttrNamesNoLeafAccess_KeyAdded_CacheMiss)
{
    // attrNames enumerates keys without forcing values -> only Content dep.
    // Any file change invalidates (no StructuredContent override).
    TempJsonFile file(R"({"x": 1, "y": 2, "z": 3})");
    auto expr = std::format("builtins.attrNames ({})", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
            << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    file.modify(R"({"w": 0, "x": 1, "y": 2, "z": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(DepPrecisionAttrKeysTest, AttrNamesOnly_KeySetChange_CacheMiss)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("builtins.attrNames ({})", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
            << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    file.modify(R"({"a": 1, "b": 2, "c": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys fails
    }
}

TEST_F(DepPrecisionAttrKeysTest, AttrNamesOnly_ValueChange_CacheHit)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("builtins.attrNames ({})", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
            << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    file.modify(R"({"a": 99, "b": 88})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // #keys passes
    }
}

TEST_F(DepPrecisionAttrKeysTest, AttrNamesPlusValue_KeySetChange_CacheMiss)
{
    TempJsonFile file(R"({"a": "alpha", "b": "beta"})");
    auto expr = std::format(
        "let j = {}; in (builtins.concatStringsSep \",\" (builtins.attrNames j)) + \":\" + j.a",
        fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
            << dumpDeps(deps);
        EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "a"))
            << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a,b:alpha"));
    }

    file.modify(R"({"a": "alpha", "b": "beta", "c": "gamma"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys fails
        EXPECT_THAT(v, IsStringEq("a,b,c:alpha"));
    }
}

TEST_F(DepPrecisionAttrKeysTest, NestedAttrNames_KeyAdded_CacheMiss)
{
    TempJsonFile file(R"({"inner": {"x": 1, "y": 2}})");
    auto expr = std::format("builtins.attrNames ({}).inner", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
            << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    file.modify(R"({"inner": {"x": 1, "y": 2, "z": 3}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys fails
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Cache behavior: attrValues (from builtins-access.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionAttrKeysTest, AttrValues_KeyAdded_CacheMiss)
{
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = std::format(
        "let j = {}; in builtins.concatStringsSep \",\" (builtins.attrValues j)",
        fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
            << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x,y"));
    }

    file.modify(R"({"a":"x","b":"y","c":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("x,y,z"));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Cache behavior: toJSON serialization (from builtins-access.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionAttrKeysTest, ToJSON_ArrayGrows_CacheMiss)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = std::format("builtins.toJSON ({})", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
            << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nString);
    }

    file.modify(R"({"items":["a","b","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(DepPrecisionAttrKeysTest, ToJSON_KeyAdded_CacheMiss)
{
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = std::format("builtins.toJSON ({})", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
            << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nString);
    }

    file.modify(R"({"a":"x","b":"y","c":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(DepPrecisionAttrKeysTest, ToJSON_OutPath_CacheHit)
{
    TempJsonFile file(R"({"name":"foo","other":"bar"})");
    auto expr = std::format(
        "let j = {}; in builtins.toJSON {{ outPath = j.name; extra = j; }}",
        fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "name"))
            << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("\"foo\""));
    }

    file.modify(R"({"name":"foo","other":"baz"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("\"foo\""));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Cache behavior: toXML (from builtins-misc.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionAttrKeysTest, ToXML_KeyAdded_CacheMiss)
{
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = std::format("builtins.toXML ({})", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
            << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nString);
    }

    file.modify(R"({"a":"x","b":"y","c":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys dep fails from printValueAsXML
    }
}

// ═══════════════════════════════════════════════════════════════════════
// TOML key set shape deps (from shape-core.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionAttrKeysTest, TOML_KeySetChange_CacheMiss)
{
    TempTomlFile file("[section]\na = 1\nb = 2\n");
    auto expr = std::format("builtins.attrNames ({}).section", ft(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
            << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    file.modify("[section]\na = 1\nb = 2\nc = 3\n");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys fails
    }
}

// ═══════════════════════════════════════════════════════════════════════
// mapAttrs / attrNames interaction (from shape-core.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionAttrKeysTest, MapAttrsNoForce_KeyAdded_CacheMiss)
{
    // mapAttrs iterates keys but doesn't force values -> only Content dep.
    TempJsonFile file(R"({"a": "1", "b": "2"})");
    auto expr = std::format("builtins.mapAttrs (n: v: v + \"-mapped\") ({})", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::ImplicitShape, shapePred("keys")))
            << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nAttrs);
    }

    file.modify(R"({"a": "1", "b": "2", "c": "3"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(DepPrecisionAttrKeysTest, MapAttrsValueAccess_UnusedKeyAdded_CacheHit)
{
    TempJsonFile file(R"({"a": "stable", "b": "other"})");
    auto expr = std::format("(builtins.mapAttrs (n: v: v + \"-mapped\") ({})).a", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "a"))
            << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable-mapped"));
    }

    file.modify(R"({"a": "stable", "b": "other", "c": "new!!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("stable-mapped"));
    }

    // Change accessed value -> re-eval
    file.modify(R"({"a": "CHANGED", "b": "other", "c": "new!!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("CHANGED-mapped"));
    }
}

TEST_F(DepPrecisionAttrKeysTest, MapAttrsRemoveUnusedKey_CacheHit)
{
    TempJsonFile file(R"({"nixpkgs": {"rev": "abc123"}, "unused-input": {"rev": "def456"}})");
    auto expr = std::format("(builtins.mapAttrs (n: v: v.rev) ({})).nixpkgs", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "rev"))
            << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("abc123"));
    }

    file.modify(R"({"nixpkgs": {"rev": "abc123"}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("abc123"));
    }
}

} // namespace nix::eval_trace
