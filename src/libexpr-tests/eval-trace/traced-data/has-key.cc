#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ═══════════════════════════════════════════════════════════════════════
// #has:key dep recording — soundness tests (must re-evaluate)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, HasKey_True_KeyRemoved)
{
    // hasAttr "x" returns true, then "x" is removed → must re-evaluate
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = R"(builtins.hasAttr "x" (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()))";

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

TEST_F(TracedDataTest, HasKey_False_KeyAdded)
{
    // hasAttr "x" returns false, then "x" is added → must re-evaluate
    TempJsonFile file(R"({"y": 2})");
    auto expr = R"(builtins.hasAttr "x" (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()))";

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

TEST_F(TracedDataTest, HasKey_SelectDefault_KeyAppears)
{
    // data.x or "d" with x absent, then x is added → must re-evaluate
    TempJsonFile file(R"({"y": 2})");
    auto expr = R"nix((builtins.fromJSON (builtins.readFile )nix"
        + file.path.string() + R"nix()).x or "default")nix";

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

TEST_F(TracedDataTest, HasKey_QuestionMark_Nested_KeyRemoved)
{
    // data ? x.y with y present, then y removed → must re-evaluate
    TempJsonFile file(R"({"x": {"y": 1}})");
    auto expr = R"nix((builtins.fromJSON (builtins.readFile )nix"
        + file.path.string() + R"nix()) ? x.y)nix";

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
// #has:key dep recording — precision tests (should serve from cache)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, HasKey_True_SiblingAdded)
{
    // hasAttr "x" true, add unrelated "z" → cache hit (precision win over #keys)
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = R"(builtins.hasAttr "x" (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()))";

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

TEST_F(TracedDataTest, HasKey_True_SiblingRemoved)
{
    // hasAttr "x" true, remove unrelated "y" → cache hit (precision win over #keys)
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = R"(builtins.hasAttr "x" (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()))";

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

TEST_F(TracedDataTest, HasKey_True_ValueChanged)
{
    // hasAttr "x" true, change x's value → cache hit (existence unchanged)
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = R"(builtins.hasAttr "x" (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()))";

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

TEST_F(TracedDataTest, HasKey_False_OtherKeyRemoved)
{
    // hasAttr "x" false, remove unrelated "y" → cache hit (precision win over #keys)
    TempJsonFile file(R"({"y": 2, "z": 3})");
    auto expr = R"(builtins.hasAttr "x" (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()))";

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

TEST_F(TracedDataTest, HasKey_SelectDefault_SiblingAdded)
{
    // data.x or "d" with x absent, add unrelated "z" → cache hit
    TempJsonFile file(R"({"y": 2})");
    auto expr = R"nix((builtins.fromJSON (builtins.readFile )nix"
        + file.path.string() + R"nix()).x or "default")nix";

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

TEST_F(TracedDataTest, HasKey_Propagated_MapAttrs)
{
    // hasAttr "x" on mapAttrs output, add sibling → cache hit (provenance propagated)
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = R"(builtins.hasAttr "x" (builtins.mapAttrs (n: v: v) (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"())))";

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
// Multi-provenance tests
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, MultiProv_Update_BothTracked_AttrNames)
{
    // a // b where both are tracked, attrNames on result, change one file's keys → re-eval
    TempJsonFile fileA(R"({"x": 1})");
    TempJsonFile fileB(R"({"y": 2})");
    auto expr = R"(builtins.attrNames ((builtins.fromJSON (builtins.readFile )"
        + fileA.path.string() + R"()) // (builtins.fromJSON (builtins.readFile )"
        + fileB.path.string() + R"())))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    // Change fileB's keys — attrNames should re-evaluate
    fileB.modify(R"({"y": 2, "z": 3})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(TracedDataTest, MultiProv_IntersectAttrs_BothTracked)
{
    // intersectAttrs both tracked, attrNames result, change one file's keys → re-eval
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"x": 10, "z": 30})");
    auto expr = R"(builtins.attrNames (builtins.intersectAttrs (builtins.fromJSON (builtins.readFile )"
        + fileA.path.string() + R"()) (builtins.fromJSON (builtins.readFile )"
        + fileB.path.string() + R"())))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    // Change fileA's keys — intersection changes
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
// Regression tests — existing blocking behaviors preserved
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, HasKey_AttrNames_StillBlocking)
{
    // attrNames records #keys (not #has), add key → must re-evaluate
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = R"(builtins.attrNames (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()))";

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
        EXPECT_EQ(loaderCalls, 1); // #keys blocks — must re-evaluate
    }
}

TEST_F(TracedDataTest, HasKey_EqOp_StillBlocking)
{
    // == on attrsets records #keys, add key → must re-evaluate
    TempJsonFile file(R"({"a": 1})");
    auto expr = R"nix((builtins.fromJSON (builtins.readFile )nix"
        + file.path.string() + R"nix()) == { a = 1; })nix";

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
        EXPECT_EQ(loaderCalls, 1); // #keys blocks — must re-evaluate
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Edge cases — fragile areas that could break in refactors
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, HasKey_TopLevel_RootObject)
{
    // #has:key on root object (empty dataPath) — tests that empty parent path
    // navigation works correctly in computeCurrentHash
    TempJsonFile file(R"({"x": 1})");
    auto expr = R"(builtins.hasAttr "x" (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    // Add sibling, x still exists → cache hit
    file.modify(R"({"x": 1, "z": 99})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // #has:x still true at root
        EXPECT_THAT(v, IsTrue());
    }
}

TEST_F(TracedDataTest, HasKey_NestedQuestionMark_IntermediateSucceeds)
{
    // data ? x.y where x exists but y doesn't — only the failure point (y)
    // records #has:y, not intermediate levels. Tests that removing x (which
    // changes the Content dep) correctly forces re-evaluation even though
    // no #has:x was recorded for the intermediate level.
    TempJsonFile file(R"({"x": {"z": 1}})");
    auto expr = R"nix((builtins.fromJSON (builtins.readFile )nix"
        + file.path.string() + R"nix()) ? x.y)nix";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsFalse()); // x exists but y doesn't
    }

    // Remove x entirely — Content dep changes, must re-evaluate
    file.modify(R"({"w": 1})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsFalse()); // still false (neither x nor y)
    }
}

TEST_F(TracedDataTest, HasKey_QuestionMark_SingleLevel_SiblingAdded)
{
    // data ? x (single level), add sibling → cache hit (precision)
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = R"nix((builtins.fromJSON (builtins.readFile )nix"
        + file.path.string() + R"nix()) ? x)nix";

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
        EXPECT_EQ(loaderCalls, 0); // #has:x still true, sibling irrelevant
        EXPECT_THAT(v, IsTrue());
    }
}

TEST_F(TracedDataTest, HasKey_SelectDefault_NestedPath_SiblingAdded)
{
    // data.inner.x or "d" where inner.x exists, add sibling to inner → cache hit
    TempJsonFile file(R"({"inner": {"x": "val", "y": 2}})");
    auto expr = R"nix((builtins.fromJSON (builtins.readFile )nix"
        + file.path.string() + R"nix()).inner.x or "default")nix";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("val")); // x exists, no default
    }

    // Add sibling z to inner, x unchanged → cache hit (scalar dep on x passes)
    file.modify(R"({"inner": {"x": "val", "y": 2, "z": 99}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // x still "val", override applies
        EXPECT_THAT(v, IsStringEq("val"));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// TOML #has:key tests
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, HasKey_Toml_True_SiblingAdded)
{
    TempTomlFile file("[section]\nx = 1\ny = 2\n");
    auto expr = R"(builtins.hasAttr "x" (builtins.fromTOML (builtins.readFile )"
        + file.path.string() + R"()).section)";

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
        EXPECT_EQ(loaderCalls, 0); // #has:x passes, z is irrelevant
        EXPECT_THAT(v, IsTrue());
    }
}

TEST_F(TracedDataTest, HasKey_Toml_True_KeyRemoved)
{
    TempTomlFile file("[section]\nx = 1\ny = 2\n");
    auto expr = R"(builtins.hasAttr "x" (builtins.fromTOML (builtins.readFile )"
        + file.path.string() + R"()).section)";

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
        EXPECT_EQ(loaderCalls, 1); // x removed → re-eval
        EXPECT_THAT(v, IsFalse());
    }
}

// ═══════════════════════════════════════════════════════════════════════
// readDir #has:key tests
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, HasKey_ReadDir_True_SiblingAdded)
{
    TempDir dir;
    dir.addFile("foo", "content");
    dir.addFile("bar", "content");

    auto expr = R"(builtins.hasAttr "foo" (builtins.readDir )" + dir.path().string() + R"())";

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
        EXPECT_EQ(loaderCalls, 0); // #has:foo passes, baz irrelevant
        EXPECT_THAT(v, IsTrue());
    }
}

TEST_F(TracedDataTest, HasKey_ReadDir_False_SiblingRemoved)
{
    TempDir dir;
    dir.addFile("bar", "content");

    auto expr = R"(builtins.hasAttr "missing" (builtins.readDir )" + dir.path().string() + R"())";

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
        EXPECT_EQ(loaderCalls, 0); // #has:missing still false
        EXPECT_THAT(v, IsFalse());
    }
}

TEST_F(TracedDataTest, HasKey_ReadDir_True_KeyRemoved)
{
    TempDir dir;
    dir.addFile("foo", "content");
    dir.addFile("bar", "content");

    auto expr = R"(builtins.hasAttr "foo" (builtins.readDir )" + dir.path().string() + R"())";

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
        EXPECT_EQ(loaderCalls, 1); // foo removed → re-eval
        EXPECT_THAT(v, IsFalse());
    }
}

// ═══════════════════════════════════════════════════════════════════════
// ? operator edge case
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, HasOp_NonAttrsetIntermediate)
{
    // data ? x.y where x is a string, not an attrset → false
    // Then change x to {"y":1} → true
    TempJsonFile file(R"({"x": "not-an-object"})");
    auto expr = R"nix((builtins.fromJSON (builtins.readFile )nix"
        + file.path.string() + R"nix()) ? x.y)nix";

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

} // namespace nix::eval_trace
