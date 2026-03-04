#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/data/traced-data.hh"
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
        EXPECT_EQ(v.attrs()->size(), 0u);
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
    // The // operator does NOT record #keys (precision fix). Only .hello
    // access records #has:hello, which passes when "newfile" is added.
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
        EXPECT_EQ(loaderCalls, 0); // #has:hello passes — newfile is unrelated
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

} // namespace nix::eval_trace
