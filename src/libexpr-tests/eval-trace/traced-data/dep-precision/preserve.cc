#include "eval-trace/helpers.hh"

#include <format>
#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/types.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DepPrecisionPreserveTest : public DepPrecisionTest {};

// ═══════════════════════════════════════════════════════════════════════
// Dep verification: // does NOT record SC #keys
// (from dep-precision-update.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionPreserveTest, Update_NoSCKeysRecorded_SingleProvenance)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({} // {{ c = 3; }}).a", fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::StructuredProjection, "a"))
        << "Scalar dep for .a must exist\n" << dumpDeps(deps);
    EXPECT_TRUE(hasJsonDep(deps, CanonicalQueryKind::ImplicitStructure, shapePred("keys")))
        << "IS #keys from creation must exist\n" << dumpDeps(deps);
    EXPECT_FALSE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
        << "// must NOT record SC #keys\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionPreserveTest, Update_NoSCKeysRecorded_MultiProvenance)
{
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"z": 3})");
    auto expr = std::format("({} // {}).x", fj(fileA.path), fj(fileB.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::StructuredProjection, "x"))
        << dumpDeps(deps);
    EXPECT_GE(countJsonDeps(deps, CanonicalQueryKind::ImplicitStructure, shapePred("keys")), 2u)
        << dumpDeps(deps);
    EXPECT_FALSE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
        << "// must NOT record SC #keys for either operand\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionPreserveTest, Update_NoSCKeysRecorded_Chained)
{
    TempJsonFile fileA(R"({"x": 1})");
    TempJsonFile fileB(R"({"y": 2})");
    TempJsonFile fileC(R"({"z": 3})");
    auto expr = std::format("({} // {} // {}).x",
        fj(fileA.path), fj(fileB.path), fj(fileC.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::StructuredProjection, "x"));
    EXPECT_FALSE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
        << "Chained // must NOT record SC #keys\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionPreserveTest, Update_NoSCKeys_NoSCType)
{
    // New: verify // doesn't record #type either
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({} // {{ c = 3; }}).a", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_FALSE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("type")))
        << "// must NOT record SC #type\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionPreserveTest, Preserve_MapAttrs_NoSCKeysDep)
{
    // New: mapAttrs value access does not record SC #keys
    TempJsonFile file(R"({"a": "x", "b": "y"})");
    auto expr = std::format(
        R"((builtins.mapAttrs (n: v: v + "!") ({})).a)", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::StructuredProjection, "a"))
        << dumpDeps(deps);
    EXPECT_FALSE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
        << "mapAttrs value access must NOT record SC #keys\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionPreserveTest, GetAttr_NoSCKeys_NoSCHasKey)
{
    // New: simple .x access records neither SC #keys nor SC #has:x
    TempJsonFile file(R"({"x": 42, "y": 99})");
    auto expr = std::format("({}).x", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::StructuredProjection, "x"))
        << dumpDeps(deps);
    EXPECT_FALSE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
        << ".x must NOT record SC #keys\n" << dumpDeps(deps);
    EXPECT_FALSE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, [](const nlohmann::json & j) { return j.contains("h"); }))
        << ".x must NOT record SC #has:*\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// Positive: consumer sites DO record the correct deps
// (from dep-precision-update.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionPreserveTest, Update_AttrNames_RecordsSCKeys)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("builtins.attrNames ({} // {{ c = 3; }})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
        << "attrNames must record SC #keys\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionPreserveTest, Update_ScalarAccess_RecordsScalarDep)
{
    TempJsonFile file(R"({"a": "hello", "b": 2})");
    auto expr = std::format("({} // {{ c = 3; }}).a", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    size_t scalarDeps = 0;
    for (auto & d : deps) {
        if (d.type == CanonicalQueryKind::StructuredProjection
            && d.key.find("#keys") == std::string::npos
            && d.key.find("#has:") == std::string::npos
            && d.key.find("#len") == std::string::npos
            && d.key.find("#type") == std::string::npos
            && d.key.find("a") != std::string::npos)
            scalarDeps++;
    }
    EXPECT_GE(scalarDeps, 1u)
        << "Scalar access .a must record a SC value dep\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// Cache: // precision (from dep-precision-update.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionPreserveTest, Update_KeyAdded_ScalarAccess_CacheHit)
{
    TempJsonFile file(R"({"a": "x", "b": "y"})");
    auto expr = std::format(R"(({} // {{ extra = "e"; }}).a)", fj(file.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::StructuredProjection, "a"))
            << dumpDeps(deps);
        EXPECT_FALSE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
            << "// must NOT record SC #keys\n" << dumpDeps(deps);
    }

    // -- Cache behavior --
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
        EXPECT_EQ(loaderCalls, 0) << "Adding unrelated key must not invalidate .a access";
        EXPECT_THAT(v, IsStringEq("x"));
    }
}

TEST_F(DepPrecisionPreserveTest, Update_KeyAdded_AttrNames_CacheMiss)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("builtins.attrNames ({} // {{ c = 3; }})", fj(file.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
            << "attrNames must record SC #keys\n" << dumpDeps(deps);
    }

    // -- Cache behavior --
    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    file.modify(R"({"a": 1, "b": 2, "d": 4})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1) << "attrNames must detect key addition via #keys";
    }
}

TEST_F(DepPrecisionPreserveTest, Update_MultiProv_ImplicitShapeFallback_CacheHit)
{
    TempJsonFile fileA(R"({"x": 1})");
    TempJsonFile fileB(R"({"y": 2})");
    auto expr = std::format("({} // {}).x", fj(fileA.path), fj(fileB.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::StructuredProjection, "x"))
            << dumpDeps(deps);
        EXPECT_GE(countJsonDeps(deps, CanonicalQueryKind::ImplicitStructure, shapePred("keys")), 2u)
            << dumpDeps(deps);
    }

    // -- Cache behavior --
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
        EXPECT_EQ(loaderCalls, 0) << "ImplicitShape fallback: b's keys unchanged";
        EXPECT_THAT(v, IsIntEq(1));
    }
}

TEST_F(DepPrecisionPreserveTest, Update_MultiProv_ImplicitShapeFallback_KeyChange_CacheMiss)
{
    TempJsonFile fileA(R"({"x": 1})");
    TempJsonFile fileB(R"({"y": 2})");
    auto expr = std::format("({} // {}).x", fj(fileA.path), fj(fileB.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::StructuredProjection, "x"))
            << dumpDeps(deps);
    }

    // -- Cache behavior --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    fileB.modify(R"({"y": 2, "x": 99})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1) << "ImplicitShape fallback: b's keys changed";
        EXPECT_THAT(v, IsIntEq(99));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// removeAttrs attrNames (from shape-modified.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionPreserveTest, RemoveAttrs_AttrNames_KeyAdded_CacheMiss)
{
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = std::format(
        R"(builtins.attrNames (builtins.removeAttrs ({}) ["y"]))", fj(file.path));

    // -- Dep verification --
    // removeAttrs with actual removal → shapeModified → SC #keys suppressed
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_FALSE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
            << "removeAttrs with actual removal suppresses SC #keys\n" << dumpDeps(deps);
        EXPECT_TRUE(hasJsonDep(deps, CanonicalQueryKind::ImplicitStructure, shapePred("keys")))
            << "IS #keys from creation must exist\n" << dumpDeps(deps);
    }

    // -- Cache behavior --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 1u);
    }

    file.modify(R"({"x": 1, "y": 2, "z": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 2u);
    }
}

TEST_F(DepPrecisionPreserveTest, RemoveAttrs_AttrNames_NoopRemove_CacheMiss)
{
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = std::format(
        R"(builtins.attrNames (builtins.removeAttrs ({}) ["absent"]))", fj(file.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
            << "attrNames after noop removeAttrs must record SC #keys\n" << dumpDeps(deps);
    }

    // -- Cache behavior --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2u);
    }

    file.modify(R"({"x": 1, "y": 2, "z": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 3u);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// mapAttrs key preservation (from shape-modified.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionPreserveTest, MapAttrs_AttrNames_KeyAdded_CacheMiss)
{
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = std::format(
        "builtins.attrNames (builtins.mapAttrs (n: v: v) ({}))", fj(file.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
            << "attrNames after mapAttrs must record SC #keys\n" << dumpDeps(deps);
    }

    // -- Cache behavior --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2u);
    }

    file.modify(R"({"x": 1, "y": 2, "z": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 3u);
    }
}

TEST_F(DepPrecisionPreserveTest, MapAttrs_AttrNames_ValueChanged_CacheHit)
{
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = std::format(
        "builtins.attrNames (builtins.mapAttrs (n: v: v) ({}))", fj(file.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
            << "attrNames after mapAttrs must record SC #keys\n" << dumpDeps(deps);
    }

    // -- Cache behavior --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2u);
    }

    file.modify(R"({"x": 99, "y": 88})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_EQ(v.listSize(), 2u);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Positional access (from builtins-access.cc, shape-core.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionPreserveTest, Head_Append_CacheHit)
{
    TempJsonFile file(R"({"items":["stable","other"]})");
    auto expr = std::format("let j = {}; in builtins.head j.items", fj(file.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::StructuredProjection, "items"))
            << dumpDeps(deps);
    }

    // -- Cache behavior --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable"));
    }

    file.modify(R"({"items":["stable","other","new"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("stable"));
    }
}

TEST_F(DepPrecisionPreserveTest, Head_Prepend_CacheMiss)
{
    TempJsonFile file(R"({"items":["stable","other"]})");
    auto expr = std::format("let j = {}; in builtins.head j.items", fj(file.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::StructuredProjection, "items"))
            << dumpDeps(deps);
    }

    // -- Cache behavior --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable"));
    }

    file.modify(R"({"items":["new","stable","other"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("new"));
    }
}

TEST_F(DepPrecisionPreserveTest, ElemAt_Append_CacheHit)
{
    TempJsonFile file(R"({"items":["a","stable","c"]})");
    auto expr = std::format("let j = {}; in builtins.elemAt j.items 1", fj(file.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::StructuredProjection, "items"))
            << dumpDeps(deps);
    }

    // -- Cache behavior --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable"));
    }

    file.modify(R"({"items":["a","stable","c","new"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("stable"));
    }
}

TEST_F(DepPrecisionPreserveTest, Preserve_PointAccess_SurvivesKeyAddition)
{
    TempJsonFile file(R"({"x": "stable"})");
    auto expr = std::format("({}).x", fj(file.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::StructuredProjection, "x"))
            << dumpDeps(deps);
    }

    // -- Cache behavior --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable"));
    }

    file.modify(R"({"x": "stable", "y": "added!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("stable"));
    }
}

TEST_F(DepPrecisionPreserveTest, Preserve_ElemAt_SurvivesArrayGrowth)
{
    TempJsonFile file(R"(["first", "second"])");
    auto expr = std::format("builtins.elemAt ({}) 0", fj(file.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_FALSE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("len")))
            << "elemAt must NOT record SC #len\n" << dumpDeps(deps);
    }

    // -- Cache behavior --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("first"));
    }

    file.modify(R"(["first", "second", "third"])");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("first"));
    }
}

TEST_F(DepPrecisionPreserveTest, MapAttrsValueAccess_NoShapeDep_CacheHit)
{
    TempJsonFile file(R"({"a": "stable", "b": "other"})");
    auto expr = std::format(
        R"((builtins.mapAttrs (n: v: v + "!") ({})).a)", fj(file.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::StructuredProjection, "a"))
            << dumpDeps(deps);
        EXPECT_FALSE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
            << "mapAttrs value access must NOT record SC #keys\n" << dumpDeps(deps);
    }

    // -- Cache behavior --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable!"));
    }

    file.modify(R"({"a": "stable", "b": "other", "c": "new!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("stable!"));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Attrset output builtins (from builtins-access.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionPreserveTest, MapAttrs_KeyAdded_CacheHit)
{
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = std::format(
        R"((builtins.mapAttrs (n: v: v + "!") ({})).a)", fj(file.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::StructuredProjection, "a"))
            << dumpDeps(deps);
        EXPECT_FALSE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
            << "mapAttrs .a must NOT record SC #keys\n" << dumpDeps(deps);
    }

    // -- Cache behavior --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x!"));
    }

    file.modify(R"({"a":"x","b":"y","c":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("x!"));
    }
}

TEST_F(DepPrecisionPreserveTest, Update_KeyAdded_CacheHit)
{
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = std::format(
        R"(let j = {}; in (j // {{ extra = "e"; }}).a)", fj(file.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::StructuredProjection, "a"))
            << dumpDeps(deps);
        EXPECT_FALSE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
            << "// must NOT record SC #keys\n" << dumpDeps(deps);
    }

    // -- Cache behavior --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x"));
    }

    file.modify(R"({"a":"x","b":"y","c":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("x"));
    }
}

TEST_F(DepPrecisionPreserveTest, RemoveAttrs_KeyAdded_CacheHit)
{
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = std::format(
        R"(let j = {}; in (builtins.removeAttrs j ["unused"]).a)", fj(file.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::StructuredProjection, "a"))
            << dumpDeps(deps);
    }

    // -- Cache behavior --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x"));
    }

    file.modify(R"({"a":"x","b":"y","c":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("x"));
    }
}

TEST_F(DepPrecisionPreserveTest, IntersectAttrs_DisjointKeyAdded_CacheHit)
{
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = std::format(
        R"(let j = {}; in (builtins.intersectAttrs {{ a = true; }} j).a)", fj(file.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        // intersectAttrs records per-key #has deps, not #keys
        EXPECT_TRUE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, hasKeyPred("a")))
            << "intersectAttrs must record SC #has:a\n" << dumpDeps(deps);
        EXPECT_FALSE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
            << "intersectAttrs must NOT record #keys\n" << dumpDeps(deps);
    }

    // -- Cache behavior --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x"));
    }

    // Adding disjoint key "c" doesn't change the intersection or result
    file.modify(R"({"a":"x","b":"y","c":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0)
            << "Adding disjoint key should not invalidate intersection result";
        EXPECT_THAT(v, IsStringEq("x"));
    }
}

TEST_F(DepPrecisionPreserveTest, NestedFieldAccess_SiblingAdded_CacheHit)
{
    TempJsonFile file(R"({"nodes":{"root":{"inputs":{"nixpkgs":"abc"}}}})");
    auto expr = std::format("let j = {}; in j.nodes.root.inputs.nixpkgs", fj(file.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::StructuredProjection, "nixpkgs"))
            << dumpDeps(deps);
    }

    // -- Cache behavior --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("abc"));
    }

    file.modify(R"({"nodes":{"root":{"inputs":{"nixpkgs":"abc","home-manager":"def"}}}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("abc"));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Multi-source // scalar access (from shape-modified.cc)
// ═══════════════════════════════════════════════════════════════════════

// Counterexample from the RapidCheck property test:
//   A = {"A":0,"_":false,"a":"","a0":0}, B = {"a":false}
//   B.a shadows A.a; accessed key is a0 (integer 0 → 1).
// This specifically tests the case where B has an overlapping key with A
// AND the accessed key is an integer with value 0 (depHash("0") = sentinel(SentinelHash::Zero)).
TEST_F(DepPrecisionPreserveTest, MultiProv_Update_OverlappingKey_ChangeAccessedValue_CacheMiss)
{
    TempJsonFile fileA(R"({"A":0,"_":false,"a":"","a0":0})");
    TempJsonFile fileB(R"({"a":false})");
    auto expr = std::format("({} // {}).\"a0\"", fj(fileA.path), fj(fileB.path));

    // -- Dep verification: SP dep for a0 MUST exist --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::StructuredProjection, "a0"))
            << "Missing scalar SP dep for a0\n" << dumpDeps(deps);
        EXPECT_GE(countJsonDeps(deps, CanonicalQueryKind::ImplicitStructure, shapePred("keys")), 1u)
            << "Missing IS #keys dep\n" << dumpDeps(deps);
    }

    // -- Cache behavior: cold eval --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(0));
    }

    // Mutate a0: 0 → 1
    fileA.modify(R"({"A":0,"_":false,"a":"","a0":1})");
    invalidateFileCache(fileA.path);

    // Warm eval MUST miss (cache must detect value change)
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1) << "Value change must invalidate cache";
        EXPECT_THAT(v, IsIntEq(1));
    }
}

TEST_F(DepPrecisionPreserveTest, MultiProv_Update_ScalarAccess_ChangeA_CacheHit)
{
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"z": 3})");
    auto expr = std::format("({} // {}).x", fj(fileA.path), fj(fileB.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::StructuredProjection, "x"))
            << dumpDeps(deps);
    }

    // -- Cache behavior --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    fileA.modify(R"({"x": 1, "y": 99})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsIntEq(1));
    }
}

TEST_F(DepPrecisionPreserveTest, MultiProv_Update_ScalarAccess_ChangeB_CacheHit)
{
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"z": 3})");
    auto expr = std::format("({} // {}).x", fj(fileA.path), fj(fileB.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::StructuredProjection, "x"))
            << dumpDeps(deps);
    }

    // -- Cache behavior --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    fileB.modify(R"({"z": 999})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsIntEq(1));
    }
}

TEST_F(DepPrecisionPreserveTest, MultiProv_Update_ScalarAccess_ChangeX_CacheMiss)
{
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"z": 3})");
    auto expr = std::format("({} // {}).x", fj(fileA.path), fj(fileB.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::StructuredProjection, "x"))
            << dumpDeps(deps);
    }

    // -- Cache behavior --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    fileA.modify(R"({"x": 42, "y": 2})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(42));
    }
}

TEST_F(DepPrecisionPreserveTest, ChainedUpdate_ScalarAccess_ChangeB_CacheHit)
{
    TempJsonFile fileA(R"({"x": 1})");
    TempJsonFile fileB(R"({"y": 2})");
    TempJsonFile fileC(R"({"z": 3})");
    auto expr = std::format("({} // {} // {}).x",
        fj(fileA.path), fj(fileB.path), fj(fileC.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::StructuredProjection, "x"))
            << dumpDeps(deps);
    }

    // -- Cache behavior --
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
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsIntEq(1));
    }
}

} // namespace nix::eval_trace
