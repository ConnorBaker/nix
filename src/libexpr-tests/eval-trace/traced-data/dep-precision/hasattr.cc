#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/types.hh"

#include <format>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DepPrecisionHasAttrTest : public DepPrecisionTest {};

// ═══════════════════════════════════════════════════════════════════════
// Dep verification: hasAttr records #has:key
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionHasAttrTest, HasAttr_RecordsHasKey)
{
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = std::format(R"(builtins.hasAttr "x" ({}))", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("x")))
        << "hasAttr must record SC #has:x\n" << dumpDeps(deps);
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
        << "hasAttr must NOT record SC #keys\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionHasAttrTest, HasOp_KeyExists_NoSCKeys)
{
    // ? operator records #has:key, NOT #keys
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = std::format("({}) ? x", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("x")))
        << "? operator must record SC #has:x\n" << dumpDeps(deps);
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
        << "? operator must NOT record SC #keys\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// Soundness: must re-evaluate (from has-key.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionHasAttrTest, HasKey_True_KeyRemoved)
{
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = std::format(R"(builtins.hasAttr "x" ({}))", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("x")))
            << dumpDeps(deps);
    }

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

TEST_F(DepPrecisionHasAttrTest, HasKey_False_KeyAdded)
{
    TempJsonFile file(R"({"y": 2})");
    auto expr = std::format(R"(builtins.hasAttr "x" ({}))", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("x")))
            << dumpDeps(deps);
    }

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsFalse());
    }

    file.modify(R"({"x": 1, "y": 2})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsTrue());
    }
}

TEST_F(DepPrecisionHasAttrTest, HasKey_SelectDefault_KeyAppears)
{
    TempJsonFile file(R"({"y": 2})");
    auto expr = std::format(R"(({}).x or "default")", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("x")))
            << dumpDeps(deps);
    }

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("default"));
    }

    file.modify(R"({"x": "found", "y": 2})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("found"));
    }
}

TEST_F(DepPrecisionHasAttrTest, HasKey_QuestionMark_Nested_KeyRemoved)
{
    TempJsonFile file(R"({"x": {"y": 1}})");
    auto expr = std::format("({}) ? x.y", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        // ? x.y records #has:y on x's sub-path, not #has:x at root
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("y")))
            << "? x.y records #has:y on x's origin\n" << dumpDeps(deps);
    }

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    file.modify(R"({"x": {"z": 1}})");
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
// Precision: cache hit (from has-key.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionHasAttrTest, HasKey_True_SiblingAdded)
{
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = std::format(R"(builtins.hasAttr "x" ({}))", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("x")))
            << dumpDeps(deps);
    }

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
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsTrue());
    }
}

TEST_F(DepPrecisionHasAttrTest, HasKey_True_SiblingRemoved)
{
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = std::format(R"(builtins.hasAttr "x" ({}))", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("x")))
            << dumpDeps(deps);
    }

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    file.modify(R"({"x": 1})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsTrue());
    }
}

TEST_F(DepPrecisionHasAttrTest, HasKey_True_ValueChanged)
{
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = std::format(R"(builtins.hasAttr "x" ({}))", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("x")))
            << dumpDeps(deps);
    }

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    file.modify(R"({"x": 999, "y": 888})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsTrue());
    }
}

TEST_F(DepPrecisionHasAttrTest, HasKey_False_OtherKeyRemoved)
{
    TempJsonFile file(R"({"y": 2, "z": 3})");
    auto expr = std::format(R"(builtins.hasAttr "x" ({}))", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("x")))
            << dumpDeps(deps);
    }

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsFalse());
    }

    file.modify(R"({"z": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsFalse());
    }
}

TEST_F(DepPrecisionHasAttrTest, HasKey_SelectDefault_SiblingAdded)
{
    TempJsonFile file(R"({"y": 2})");
    auto expr = std::format(R"(({}).x or "default")", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("x")))
            << dumpDeps(deps);
    }

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("default"));
    }

    file.modify(R"({"y": 2, "z": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("default"));
    }
}

TEST_F(DepPrecisionHasAttrTest, HasKey_Propagated_MapAttrs)
{
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = std::format(R"(builtins.hasAttr "x" (builtins.mapAttrs (n: v: v) ({})))", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("x")))
            << dumpDeps(deps);
    }

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
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsTrue());
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Regression: existing blocking behaviors preserved (from has-key.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionHasAttrTest, HasKey_AttrNames_StillBlocking)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("builtins.attrNames ({})", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
            << dumpDeps(deps);
    }

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
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(DepPrecisionHasAttrTest, HasKey_EqOp_StillBlocking)
{
    TempJsonFile file(R"({"a": 1})");
    auto expr = std::format("({}) == {{ a = 1; }}", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
            << dumpDeps(deps);
    }

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    file.modify(R"({"a": 1, "b": 2})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Edge cases (from has-key.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionHasAttrTest, HasKey_TopLevel_RootObject)
{
    TempJsonFile file(R"({"x": 1})");
    auto expr = std::format(R"(builtins.hasAttr "x" ({}))", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("x")))
            << dumpDeps(deps);
    }

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    file.modify(R"({"x": 1, "z": 99})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsTrue());
    }
}

TEST_F(DepPrecisionHasAttrTest, HasKey_NestedQuestionMark_IntermediateSucceeds)
{
    TempJsonFile file(R"({"x": {"z": 1}})");
    auto expr = std::format("({}) ? x.y", fj(file.path));

    // ── Dep verification ──
    // ? x.y where x exists but y doesn't — records #has:y on x's sub-path
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("y")))
            << "? x.y records #has:y on x's origin\n" << dumpDeps(deps);
    }

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsFalse());
    }

    file.modify(R"({"w": 1})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsFalse());
    }
}

TEST_F(DepPrecisionHasAttrTest, HasKey_QuestionMark_SingleLevel_SiblingAdded)
{
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = std::format("({}) ? x", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("x")))
            << dumpDeps(deps);
    }

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
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsTrue());
    }
}

TEST_F(DepPrecisionHasAttrTest, HasKey_SelectDefault_NestedPath_SiblingAdded)
{
    TempJsonFile file(R"({"inner": {"x": "val", "y": 2}})");
    auto expr = std::format(R"(({}).inner.x or "default")", fj(file.path));

    // ── Dep verification ──
    // .inner.x or "default" where x exists → records scalar dep inner.x, not #has:x
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, pathContainsPred(nlohmann::json({"inner", "x"}))))
            << "or-default with existing key records scalar dep\n" << dumpDeps(deps);
    }

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("val"));
    }

    file.modify(R"({"inner": {"x": "val", "y": 2, "z": 99}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("val"));
    }
}

TEST_F(DepPrecisionHasAttrTest, HasOp_NonAttrsetIntermediate)
{
    TempJsonFile file(R"({"x": "not-an-object"})");
    auto expr = std::format("({}) ? x.y", fj(file.path));

    // ── Dep verification ──
    // ? x.y where x is a string — records scalar dep for x (forcing it
    // reveals it's not an attrset, so no #has:y is recorded)
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, pathContainsPred(nlohmann::json({"x"}))))
            << "? x.y on non-attrset records scalar dep for x\n" << dumpDeps(deps);
    }

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsFalse());
    }

    file.modify(R"({"x": {"y": 1}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsTrue());
    }
}

// ═══════════════════════════════════════════════════════════════════════
// TOML #has:key (from has-key.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionHasAttrTest, HasKey_Toml_True_SiblingAdded)
{
    TempTomlFile file("[section]\nx = 1\ny = 2\n");
    auto expr = std::format(R"(builtins.hasAttr "x" ({}).section)", ft(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("x")))
            << dumpDeps(deps);
    }

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    file.modify("[section]\nx = 1\ny = 2\nz = 3\n");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsTrue());
    }
}

TEST_F(DepPrecisionHasAttrTest, HasKey_Toml_True_KeyRemoved)
{
    TempTomlFile file("[section]\nx = 1\ny = 2\n");
    auto expr = std::format(R"(builtins.hasAttr "x" ({}).section)", ft(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("x")))
            << dumpDeps(deps);
    }

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    file.modify("[section]\ny = 2\n");
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
// readDir #has:key (from has-key.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionHasAttrTest, HasKey_ReadDir_True_SiblingAdded)
{
    TempDir dir;
    dir.addFile("foo", "content");
    dir.addFile("bar", "content");

    auto expr = std::format(R"(builtins.hasAttr "foo" ({}))", rd(dir.path()));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("foo")))
            << dumpDeps(deps);
    }

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    dir.addFile("baz", "content");
    invalidateFileCache(dir.path());

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsTrue());
    }
}

TEST_F(DepPrecisionHasAttrTest, HasKey_ReadDir_False_SiblingRemoved)
{
    TempDir dir;
    dir.addFile("bar", "content");

    auto expr = std::format(R"(builtins.hasAttr "missing" ({}))", rd(dir.path()));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("missing")))
            << dumpDeps(deps);
    }

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsFalse());
    }

    dir.removeEntry("bar");
    invalidateFileCache(dir.path());

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsFalse());
    }
}

TEST_F(DepPrecisionHasAttrTest, HasKey_ReadDir_True_KeyRemoved)
{
    TempDir dir;
    dir.addFile("foo", "content");
    dir.addFile("bar", "content");

    auto expr = std::format(R"(builtins.hasAttr "foo" ({}))", rd(dir.path()));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("foo")))
            << dumpDeps(deps);
    }

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    dir.removeEntry("foo");
    invalidateFileCache(dir.path());

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsFalse());
    }
}

// ═══════════════════════════════════════════════════════════════════════
// ? operator / or expression cache behavior (from builtins-access.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionHasAttrTest, HasOp_KeyAdded_CacheMiss)
{
    TempJsonFile file(R"({"a":"x"})");
    auto expr = std::format(R"(let j = {}; in (if j ? b then j.b else "default") + "-" + j.a)", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("b")))
            << dumpDeps(deps);
    }

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("default-x"));
    }

    file.modify(R"({"a":"x","b":"y"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("y-x"));
    }
}

TEST_F(DepPrecisionHasAttrTest, HasOp_KeyRemoved_CacheMiss)
{
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = std::format(R"(let j = {}; in (if j ? b then j.b else "default") + "-" + j.a)", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("b")))
            << dumpDeps(deps);
    }

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("y-x"));
    }

    file.modify(R"({"a":"x"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("default-x"));
    }
}

TEST_F(DepPrecisionHasAttrTest, HasOp_KeyUnchanged_ScalarChanged_CacheMiss)
{
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = std::format(R"(let j = {}; in (if j ? a then j.a else "default") + "-" + j.b)", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("a")))
            << dumpDeps(deps);
    }

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x-y"));
    }

    file.modify(R"({"a":"x","b":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // scalar dep .b changed
        EXPECT_THAT(v, IsStringEq("x-z"));
    }
}

TEST_F(DepPrecisionHasAttrTest, SelectOrDefault_KeyAdded_CacheMiss)
{
    TempJsonFile file(R"({"a":"x"})");
    auto expr = std::format(R"(let j = {}; in (j.b or "default") + "-" + j.a)", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("b")))
            << dumpDeps(deps);
    }

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("default-x"));
    }

    file.modify(R"({"a":"x","b":"y"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("y-x"));
    }
}

TEST_F(DepPrecisionHasAttrTest, SelectOrDefault_KeyPresent_CacheHit)
{
    TempJsonFile file(R"({"a":"x","b":"y","c":"z"})");
    auto expr = std::format(R"(let j = {}; in (j.b or "default") + "-" + j.a)", fj(file.path));

    // ── Dep verification ──
    // .b or "default" where b exists → records scalar deps for b and a, not #has:b
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, pathContainsPred(nlohmann::json({"b"}))))
            << "or-default with existing key records scalar dep for b\n" << dumpDeps(deps);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, pathContainsPred(nlohmann::json({"a"}))))
            << "scalar dep for a\n" << dumpDeps(deps);
    }

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("y-x"));
    }

    file.modify(R"({"a":"x","b":"y","c":"w"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("y-x"));
    }
}

} // namespace nix::eval_trace
