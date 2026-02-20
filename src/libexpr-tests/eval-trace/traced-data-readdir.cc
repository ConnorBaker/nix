#include "helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/traced-data.hh"
#include "nix/expr/json-to-value.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ═══════════════════════════════════════════════════════════════════════
// readDir ExprTracedData tests — Directory two-level override
// ═══════════════════════════════════════════════════════════════════════

// ── Group 1: Basic ExprTracedData for readDir ──────────────────────────

TEST_F(TracedDataTest, TracedDir_BasicAccess)
{
    TempDir td;
    td.addFile("hello", "content");
    td.addSubdir("world");
    auto expr = "(builtins.readDir " + td.path().string() + ").hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_MultipleAccess)
{
    TempDir td;
    td.addFile("hello", "content");
    td.addSubdir("world");
    auto expr = "(builtins.readDir " + td.path().string() + ").hello + \"-\" + (builtins.readDir " + td.path().string() + ").world";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular-directory"));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("regular-directory"));
    }
}

TEST_F(TracedDataTest, TracedDir_SymlinkEntry)
{
    TempDir td;
    td.addFile("target", "x");
    td.addSymlink("link", "target");
    auto expr = "(builtins.readDir " + td.path().string() + ").link";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("symlink"));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("symlink"));
    }
}

TEST_F(TracedDataTest, TracedDir_DirectoryEntry)
{
    TempDir td;
    td.addSubdir("subdir");
    td.addFile("file", "x");
    auto expr = "(builtins.readDir " + td.path().string() + ").subdir";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("directory"));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("directory"));
    }
}

TEST_F(TracedDataTest, TracedDir_SpecialCharKey)
{
    // Entry name contains '.' → data path escaping must handle it
    TempDir td;
    td.addFile("foo.bar", "x");
    td.addFile("other", "y");
    auto expr = "(builtins.readDir " + td.path().string() + ").\"foo.bar\"";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_EmptyDir)
{
    TempDir td;
    auto expr = "builtins.readDir " + td.path().string();

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nAttrs);
        EXPECT_EQ(v.attrs()->size(), 0);
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_TRUE(v.type() == nAttrs);
    }
}

// ── Group 2: Two-Level Override — Positive Cases ───────────────────────

TEST_F(TracedDataTest, TracedDir_AddUnrelatedFile)
{
    TempDir td;
    td.addFile("hello", "content");
    td.addFile("other", "pad");
    auto expr = "(builtins.readDir " + td.path().string() + ").hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    // Add new file "newfile" — unrelated to accessed "hello"
    td.addFile("newfile", "new-content");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // trace valid (two-level override)
        EXPECT_THAT(v, IsStringEq("regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_RemoveUnrelatedFile)
{
    TempDir td;
    td.addFile("hello", "content");
    td.addFile("other", "pad-pad");
    auto expr = "(builtins.readDir " + td.path().string() + ").hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    td.removeEntry("other");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_ChangeUnrelatedType)
{
    TempDir td;
    td.addFile("hello", "content");
    td.addFile("other", "regular-file");
    auto expr = "(builtins.readDir " + td.path().string() + ").hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    // Change "other" from regular → symlink
    td.changeToSymlink("other", "hello");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_ThroughUpdate)
{
    // readDir result passed through // (update operator) — thunks survive.
    // The // operator records #keys for both operands, so adding a new file
    // causes a #keys dep failure even though .hello is unchanged. This is the
    // expected precision trade-off for the soundness fix in ExprOpUpdate.
    TempDir td;
    td.addFile("hello", "content");
    td.addFile("other", "pad");
    auto expr = "((builtins.readDir " + td.path().string() + ") // { extra = \"val\"; }).hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    td.addFile("newfile", "new-content");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys dep on readDir result causes re-eval
        EXPECT_THAT(v, IsStringEq("regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_ThroughMapAttrs)
{
    // readDir result passed through mapAttrs — thunks survive
    TempDir td;
    td.addFile("hello", "content");
    td.addFile("other", "pad");
    auto expr = "let d = builtins.mapAttrs (n: v: v) (builtins.readDir " + td.path().string() + "); in d.hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    td.addFile("newfile", "new-content");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_MultiAccessOneChanges)
{
    TempDir td;
    td.addFile("a", "x");
    td.addFile("b", "y");
    td.addFile("other", "z");
    auto expr = "let d = builtins.readDir " + td.path().string()
        + "; in d.a + \"-\" + d.b";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular-regular"));
    }

    // Add unrelated file — both accessed entries unchanged
    td.addFile("c", "new");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("regular-regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_AddMultipleFiles)
{
    TempDir td;
    td.addFile("hello", "content");
    auto expr = "(builtins.readDir " + td.path().string() + ").hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    td.addFile("new1", "a");
    td.addFile("new2", "b");
    td.addFile("new3", "c");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_MixedContentAndDir)
{
    // readFile (fromJSON) + readDir, change unused JSON key AND add file to dir
    TempJsonFile jf(R"({"used": "stable", "unused": "original"})");
    TempDir td;
    td.addFile("hello", "content");
    td.addFile("other", "pad");

    auto expr = "let j = builtins.fromJSON (builtins.readFile " + jf.path.string()
        + "); d = builtins.readDir " + td.path().string()
        + "; in j.used + \"-\" + d.hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable-regular"));
    }

    // Change unused JSON key AND add file to dir
    jf.modify(R"({"used": "stable", "unused": "changed-value!!"})");
    invalidateFileCache(jf.path);
    td.addFile("newfile", "new");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // both Content and Directory override
        EXPECT_THAT(v, IsStringEq("stable-regular"));
    }
}

// ── Group 3: Two-Level Override — Negative Cases ───────────────────────

TEST_F(TracedDataTest, TracedDir_AccessedEntryTypeChanges)
{
    TempDir td;
    td.addFile("hello", "content");
    td.addFile("other", "pad");
    auto expr = "(builtins.readDir " + td.path().string() + ").hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    // Change "hello" from regular → symlink
    td.changeToSymlink("hello", "other");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // trace invalid (SC dep for hello fails)
        EXPECT_THAT(v, IsStringEq("symlink"));
    }
}

TEST_F(TracedDataTest, TracedDir_AccessedEntryRemoved)
{
    TempDir td;
    td.addFile("hello", "content");
    td.addFile("other", "pad-pad");
    auto expr = "(builtins.readDir " + td.path().string() + ").hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    td.removeEntry("hello");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        // Re-eval: .hello missing → throws
        EXPECT_THROW(forceRoot(*cache), Error);
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(TracedDataTest, TracedDir_NoValuesForced)
{
    // attrNames only — no per-entry SC deps recorded
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
        EXPECT_EQ(loaderCalls, 1); // no SC deps → no override
    }
}

TEST_F(TracedDataTest, TracedDir_AllEntriesChangedType)
{
    TempDir td;
    td.addFile("a", "x");
    td.addFile("b", "y");
    auto expr = "let d = builtins.readDir " + td.path().string()
        + "; in d.a + \"-\" + d.b";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular-regular"));
    }

    // Change both to directories
    td.changeToSubdir("a");
    td.changeToSubdir("b");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("directory-directory"));
    }
}

TEST_F(TracedDataTest, TracedDir_OneAccessedOneChanged)
{
    TempDir td;
    td.addFile("a", "x");
    td.addFile("b", "y");
    auto expr = "let d = builtins.readDir " + td.path().string()
        + "; in d.a + \"-\" + d.b";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular-regular"));
    }

    // Change only b's type
    td.changeToSubdir("b");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // SC dep for b fails
        EXPECT_THAT(v, IsStringEq("regular-directory"));
    }
}

TEST_F(TracedDataTest, TracedDir_MapAttrsIgnoreValues)
{
    // mapAttrs (n: _: n) — ignores values entirely, no SC deps
    TempDir td;
    td.addFile("a", "x");
    td.addFile("b", "y");
    auto expr = "builtins.mapAttrs (n: _: n) (builtins.readDir " + td.path().string() + ")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nAttrs);
    }

    td.addFile("c", "z");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // no SC deps → no override
    }
}

TEST_F(TracedDataTest, TracedDir_AccessedEntryReplacedSameType)
{
    // Replace "hello" with different content but same type (regular → regular)
    TempDir td;
    td.addFile("hello", "content-A");
    td.addFile("other", "pad");
    auto expr = "(builtins.readDir " + td.path().string() + ").hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    // Re-create hello with different content but same type
    td.removeEntry("hello");
    td.addFile("hello", "content-B-different");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        // Type unchanged ("regular" = "regular") → trace valid
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_MixedInvalid)
{
    // readFile (fromJSON) + readDir, accessed JSON leaf changes
    TempJsonFile jf(R"({"used": "hello", "extra": "x"})");
    TempDir td;
    td.addFile("a", "content");

    auto expr = "let j = builtins.fromJSON (builtins.readFile " + jf.path.string()
        + "); d = builtins.readDir " + td.path().string()
        + "; in j.used + \"-\" + d.a";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello-regular"));
    }

    // Change accessed JSON leaf
    jf.modify(R"({"used": "CHANGED!!", "extra": "x"})");
    invalidateFileCache(jf.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // SC dep for used fails
        EXPECT_THAT(v, IsStringEq("CHANGED!!-regular"));
    }
}

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
