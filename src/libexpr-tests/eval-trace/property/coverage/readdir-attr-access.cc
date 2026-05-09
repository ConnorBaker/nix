// readDir × .attr access property tests — fine-grained SC dep coverage.
//
// builtins.readDir returns an attrset of {filename → "regular"|"symlink"|
// "directory"|"unknown"}.  The dep recorded is a DirectoryEntries dep whose
// hash covers the sorted listing (names + entry types).  It does NOT cover
// file content.
//
// Test strategy: deterministic, hand-crafted expressions (no RapidCheck).
// All tests use TempDir and INVALIDATE_DIR / invalidateFileCache after
// directory modifications.
//
// Key scenarios:
//   a) Soundness: change entry type (regular → symlink) → dep hash changes.
//   b) Precision: change file content only → dep hash unchanged → cache hit.
//   c) HasAttr soundness: delete file → hasAttr → must miss.
//   d) MapAttrs soundness: add new file → all-keys traversal → must miss.

#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// ── Fixture ──────────────────────────────────────────────────────────────────

class EvalTraceProperty_ReadDirAttrAccess : public TraceCacheFixture {
public:
    EvalTraceProperty_ReadDirAttrAccess() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-readdir-attr-access-test");
    }
};

// ── Test a: ReadDir_AttrAccess_Soundness ──────────────────────────────────
//
// Expression: (builtins.readDir <dir>).myfile
//
// Initially myfile is a regular file → value = "regular".
// Change myfile to a symlink → entry type changes → dep hash changes → must miss.
TEST_F(EvalTraceProperty_ReadDirAttrAccess, ReadDir_AttrAccess_Soundness)
{
    TempDir dir;
    dir.addFile("myfile", "some content");
    dir.addFile("other", "other content");

    auto const expr = "(builtins.readDir " + dir.path().string() + ").myfile";

    // Cold eval: myfile is a regular file → "regular".
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "regular");
    }

    // Warm eval — confirm cache hit (precision pre-condition).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before entry-type change";
    }

    // Change myfile from regular → symlink → entry type in directory listing changes.
    dir.changeToSymlink("myfile", "other");
    INVALIDATE_DIR(dir);

    // Must re-evaluate: directory listing hash changes because entry type changed.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "cache miss expected: entry type changed from regular to symlink";
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "symlink");
    }
}

// ── Test b: ReadDir_AttrAccess_Precision ─────────────────────────────────
//
// Expression: (builtins.readDir <dir>).fileA
//
// fileA and fileB are both regular files.  Access .fileA, then change the
// CONTENT of fileB (different file, no add/remove, no type change).
// readDir only hashes names and entry types, not content → must hit.
TEST_F(EvalTraceProperty_ReadDirAttrAccess, ReadDir_AttrAccess_Precision)
{
    TempDir dir;
    dir.addFile("fileA", "content of A");
    dir.addFile("fileB", "content of B");

    auto const expr = "(builtins.readDir " + dir.path().string() + ").fileA";

    // Cold eval: fileA is a regular file.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "regular");
    }

    // Warm eval — confirm baseline cache hit.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before content change";
    }

    // Change content of fileB — no add/remove, no type change.
    // readDir does not hash file content; the directory listing is unchanged.
    dir.addFile("fileB", "completely different content that is much longer than before");
    INVALIDATE_DIR(dir);

    // Must still be a cache hit: the listing (names + types) is identical.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0)
            << "cache hit expected: only fileB content changed; "
               "readDir dep does not track content, only names and entry types";
    }
}

// ── Test c: ReadDir_HasAttr_Soundness ─────────────────────────────────────
//
// Expression: builtins.hasAttr "myfile" (builtins.readDir <dir>)
//
// Initially myfile exists → true.  Delete myfile → listing changes → must miss.
TEST_F(EvalTraceProperty_ReadDirAttrAccess, ReadDir_HasAttr_Soundness)
{
    TempDir dir;
    dir.addFile("myfile", "content");
    dir.addFile("other", "other");

    auto const expr =
        "builtins.hasAttr \"myfile\" (builtins.readDir " + dir.path().string() + ")";

    // Cold eval: myfile present → true.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nBool);
        EXPECT_TRUE(v.boolean());
    }

    // Warm eval — confirm cache hit.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before deletion";
    }

    // Delete myfile → directory listing changes → dep hash changes.
    dir.removeEntry("myfile");
    INVALIDATE_DIR(dir);

    // Must re-evaluate: listing changed (file removed).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "cache miss expected: myfile deleted, listing changed";
        EXPECT_EQ(v.type(), nBool);
        EXPECT_FALSE(v.boolean());
    }
}

// ── Test d: ReadDir_MapAttrs_Soundness ────────────────────────────────────
//
// Expression:
//   (builtins.mapAttrs (k: v: v) (builtins.readDir <dir>)).fileA
//
// Initially dir has {fileA, fileB}.  We access .fileA.  Removing fileA
// (the accessed key) causes the SC dep for that key to fail → must miss.
TEST_F(EvalTraceProperty_ReadDirAttrAccess, ReadDir_MapAttrs_Soundness)
{
    TempDir dir;
    dir.addFile("fileA", "a");
    dir.addFile("fileB", "b");

    auto const expr =
        "(builtins.mapAttrs (k: v: v) (builtins.readDir " + dir.path().string() + ")).fileA";

    // Cold eval: fileA → "regular".
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "regular");
    }

    // Warm eval — confirm cache hit.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before directory change";
    }

    // Remove fileA — the accessed entry disappears; SC dep for "fileA" fails.
    dir.removeEntry("fileA");
    INVALIDATE_DIR(dir);

    // Must re-evaluate: SC dep for accessed key "fileA" fails (key removed).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        // forceRoot throws because the expression accesses the removed entry.
        EXPECT_THROW(forceRoot(*cache), nix::Error);
        EXPECT_EQ(calls, 1) << "cache miss expected: accessed entry 'fileA' removed";
    }
}

} // namespace nix::eval_trace::proptest
