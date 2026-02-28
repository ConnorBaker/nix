#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/data/traced-data.hh"
#include "nix/expr/json-to-value.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── Group 4: Shape Deps for readDir ────────────────────────────────────

TEST_F(TracedDataTest, TracedDir_AttrNames_KeyAdded)
{
    TempDir td;
    td.addFile("a", "x");
    td.addFile("b", "y");
    auto expr = "builtins.attrNames (builtins.readDir " + td.path().string() + ")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    td.addFile("c", "z");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys changes
    }
}

TEST_F(TracedDataTest, TracedDir_AttrNames_KeyRemoved)
{
    TempDir td;
    td.addFile("a", "x");
    td.addFile("b", "y");
    td.addFile("c", "z");
    auto expr = "builtins.attrNames (builtins.readDir " + td.path().string() + ")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    td.removeEntry("c");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys changes
    }
}

TEST_F(TracedDataTest, TracedDir_AttrNames_TypeChanged)
{
    TempDir td;
    td.addFile("a", "x");
    td.addFile("b", "y");
    auto expr = "builtins.attrNames (builtins.readDir " + td.path().string() + ")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    // Change type of "b" — keys stay the same
    td.changeToSubdir("b");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // #keys unchanged → override applies
    }
}

TEST_F(TracedDataTest, TracedDir_HasAttr_True_KeyRemoved)
{
    TempDir td;
    td.addFile("foo", "x");
    td.addFile("bar", "y");
    auto expr = "builtins.hasAttr \"foo\" (builtins.readDir " + td.path().string() + ")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    td.removeEntry("foo");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys changes
        EXPECT_THAT(v, IsFalse());
    }
}

TEST_F(TracedDataTest, TracedDir_HasAttr_False_KeyAdded)
{
    TempDir td;
    td.addFile("bar", "x");
    auto expr = "builtins.hasAttr \"foo\" (builtins.readDir " + td.path().string() + ")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsFalse());
    }

    td.addFile("foo", "y");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys changes
        EXPECT_THAT(v, IsTrue());
    }
}

TEST_F(TracedDataTest, TracedDir_AttrNamesAndAccess_KeyAdded)
{
    TempDir td;
    td.addFile("hello", "x");
    td.addFile("other", "y");
    auto expr = "let d = builtins.readDir " + td.path().string()
        + "; in (builtins.concatStringsSep \",\" (builtins.attrNames d)) + \":\" + d.hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello,other:regular"));
    }

    td.addFile("added", "z");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys SC dep fails
        EXPECT_THAT(v, IsStringEq("added,hello,other:regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_AttrNames_ValChangedKeySame)
{
    TempDir td;
    td.addFile("a", "x");
    td.addFile("b", "y");
    auto expr = "builtins.attrNames (builtins.readDir " + td.path().string() + ")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    // Change all types but keep same entries
    td.changeToSubdir("a");
    td.changeToSymlink("b", "a");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // #keys hash only covers names
    }
}

// ── Group 5: Edge Cases ────────────────────────────────────────────────

TEST_F(TracedDataTest, TracedDir_ContainerProvLostAfterUpdate)
{
    // attrNames (readDir dir // { x = 1; }) — container provenance is lost
    // because // creates a new Bindings*, so #keys dep is NOT recorded
    TempDir td;
    td.addFile("a", "x");
    td.addFile("b", "y");
    auto expr = "builtins.attrNames (builtins.readDir " + td.path().string() + " // { x = 1; })";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    td.addFile("c", "z");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        // Container prov lost → no #keys dep → Directory dep alone, no override possible
        // BUT: the readDir value is still ExprTracedData. After //, attrNames sees
        // the merged Bindings* which has no provenance. No SC deps → must re-eval.
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(TracedDataTest, TracedDir_NestedReadDir)
{
    TempDir td1;
    td1.addFile("a", "x");
    td1.addFile("a-other", "y");
    TempDir td2;
    td2.addFile("b", "p");
    td2.addFile("b-other", "q");

    auto expr = "let d1 = builtins.readDir " + td1.path().string()
        + "; d2 = builtins.readDir " + td2.path().string()
        + "; in d1.a + \"-\" + d2.b";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular-regular"));
    }

    // Change dir1 only (add unrelated file)
    td1.addFile("new-in-d1", "z");
    INVALIDATE_DIR(td1);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // dir1 override (a unchanged), dir2 unchanged
        EXPECT_THAT(v, IsStringEq("regular-regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_LargeDirectory)
{
    TempDir td;
    // Create 100 entries
    for (int i = 0; i < 100; i++)
        td.addFile("entry-" + std::to_string(i), std::to_string(i));

    auto expr = "(builtins.readDir " + td.path().string() + ").\"entry-42\"";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    // Add more entries
    for (int i = 100; i < 110; i++)
        td.addFile("entry-" + std::to_string(i), std::to_string(i));
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // override applies (entry-42 unchanged)
        EXPECT_THAT(v, IsStringEq("regular"));
    }
}

} // namespace nix::eval_trace
