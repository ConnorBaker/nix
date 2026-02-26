#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/dependency-tracker.hh"
#include "nix/expr/eval-trace-deps.hh"

#include <format>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DepPrecisionHasAttrMultiTest : public DepPrecisionTest {};

// ═══════════════════════════════════════════════════════════════════════
// Multi-provenance hasAttr (from has-key.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionHasAttrMultiTest, MultiProv_Update_BothTracked_AttrNames)
{
    TempJsonFile fileA(R"({"x": 1})");
    TempJsonFile fileB(R"({"y": 2})");
    auto expr = std::format("builtins.attrNames ({} // {})", fj(fileA.path), fj(fileB.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    fileB.modify(R"({"y": 2, "z": 3})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(DepPrecisionHasAttrMultiTest, MultiProv_IntersectAttrs_BothTracked)
{
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"x": 10, "z": 30})");
    auto expr = std::format("builtins.attrNames (builtins.intersectAttrs ({}) ({}))", fj(fileA.path), fj(fileB.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    fileA.modify(R"({"z": 1, "y": 2})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Multi-source #has:key (from shape-modified.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionHasAttrMultiTest, MultiProv_Update_HasKey_BothHave)
{
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"x": 10, "y": 20})");
    auto expr = std::format(R"(builtins.hasAttr "x" ({} // {}))", fj(fileA.path), fj(fileB.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    fileA.modify(R"({"x": 999, "y": 888})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // ImplicitShape fallback: a's keys unchanged
        EXPECT_THAT(v, IsTrue());
    }
}

TEST_F(DepPrecisionHasAttrMultiTest, MultiProv_Update_HasKey_OnlyAHas)
{
    TempJsonFile fileA(R"({"x": 1})");
    TempJsonFile fileB(R"({"y": 2})");
    auto expr = std::format(R"(builtins.hasAttr "x" ({} // {}))", fj(fileA.path), fj(fileB.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    fileB.modify(R"({"y": 2, "z": 3})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // shapeModified + multi-source → re-eval
        EXPECT_THAT(v, IsTrue());
    }
}

// ═══════════════════════════════════════════════════════════════════════
// removeAttrs hasAttr (from shape-modified.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionHasAttrMultiTest, RemoveAttrs_HasKey_SiblingAdded)
{
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = std::format(R"(builtins.hasAttr "x" (builtins.removeAttrs ({}) ["y"]))", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    file.modify(R"({"x": 1, "y": 2, "z": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // #has:x passes
        EXPECT_THAT(v, IsTrue());
    }
}

TEST_F(DepPrecisionHasAttrMultiTest, RemoveAttrs_HasKey_QueriedKeyRemoved)
{
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = std::format(R"(builtins.hasAttr "y" (builtins.removeAttrs ({}) ["y"]))", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsFalse());
    }

    file.modify(R"({"x": 1, "y": 999})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsFalse());
    }
}

TEST_F(DepPrecisionHasAttrMultiTest, MapAttrs_HasKey_QueriedKeyRemoved)
{
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = std::format(R"(builtins.hasAttr "x" (builtins.mapAttrs (n: v: v) ({})))", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    file.modify(R"({"y": 2})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsFalse());
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Dep verification for // hasAttr (from dep-precision-update.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionHasAttrMultiTest, Update_HasAttr_RecordsHasKey)
{
    TempJsonFile file(R"({"x": 1, "b": 2})");
    auto expr = std::format(R"(builtins.hasAttr "x" ({} // {{ c = 3; }}))", fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#has:x"))
        << "hasAttr must record SC #has:x\n" << dumpDeps(deps);
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "hasAttr + // must not produce SC #keys\n" << dumpDeps(deps);
}

} // namespace nix::eval_trace
