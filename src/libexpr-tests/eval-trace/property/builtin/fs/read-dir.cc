// readDir + mapAttrs composition property test.
//
// Pattern 3: NixOS module system discovery — readDir enumerates, mapAttrs
// processes.
//
// Expression shape:
//   (builtins.mapAttrs (name: type: "${name}:${type}") (builtins.readDir <dir>))."<filename>"
//
// Generator: makeReadDirMapAttrsGen() — creates a TempDir with 2–4
// randomly-named regular files, picks one as the accessed key, and sets
// the dirEntryName slot field to a NEW name not yet present (for mutation).
//
// Four scenarios:
//
//   1. AddEntry_Unrelated_StillHits (RC property):
//      Cold eval → warm hit → add new entry (slot.mutate("exists")) →
//      invalidateFileCache → RC_ASSERT(calls == 0).
//      The SC structural override correctly serves a cache hit because the
//      ACCESSED entry is unchanged (dirEntryName is not the accessed key).
//
//   1b. RemoveAccessedEntry_Invalidates (deterministic):
//      TempDir with "target". Cold eval accessing "target" → warm hit →
//      remove "target" → invalidateFileCache → EXPECT_EQ(calls, 1).
//      SC dep for the accessed key fails when that key is removed.
//
//   2. FileContentChange_NoDirChange_StillHits (deterministic):
//      Cold eval → warm hit → overwrite a file's content without adding/
//      removing/renaming entries → invalidateFileCache → EXPECT_EQ(calls, 0).
//      readDir only hashes names + entry types, not content.
//
//   3. RemoveEntry_Invalidates (deterministic):
//      TempDir with "a" and "b". Cold eval accessing "a" → warm hit →
//      remove "a" (the accessed key) → invalidateFileCache → EXPECT_EQ(calls, 1).

#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <fstream>

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// ── Fixture ───────────────────────────────────────────────────────────────

class EvalTraceProperty_ReadDir : public TraceCacheFixture {
public:
    EvalTraceProperty_ReadDir() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-read-dir");
    }
};

// maxSuccess = 50: each iteration does two cold evals.
static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 50;
    return params;
}

// ── Test 1: AddEntry_Unrelated_StillHits (RapidCheck) ────────────────────────
//
// Precision: adding a new directory entry whose name is NOT the accessed key
// does not invalidate the cache.  The SC structural override correctly serves
// a cache hit because the ACCESSED entry is unchanged.
TEST_F(EvalTraceProperty_ReadDir, AddEntry_Unrelated_StillHits)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto expr = *makeReadDirMapAttrsGen();
            RC_PRE(expr.expectsSuccess());
            RC_PRE(expr.depSlots[0].currentValue == "missing");

            auto & slot = expr.depSlots[0];

            // Cold eval — records trace with DirectoryEntries dep.
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // Warm eval — confirm cache hit.
            {
                int calls = 0;
                auto cache = makeCache(expr.nixCode, &calls);
                (void) forceRoot(*cache);
                RC_ASSERT(calls == 0);
            }

            // Add a new entry (dirEntryName is not the accessed key).
            slot.mutate("exists");
            invalidateFileCache(slot.path);

            // Precision: SC dep for the accessed key still verifies →
            // ValidViaStructuralOverride → cache hit.
            {
                int calls = 0;
                auto cache = makeCache(expr.nixCode, &calls);
                (void) forceRoot(*cache);
                RC_ASSERT(calls == 0);
            }

            // Restore for the next iteration.
            slot.restore();
            invalidateFileCache(slot.path);
        },
        makeParams);
}

// ── Test 1b: RemoveAccessedEntry_Invalidates (deterministic) ─────────────────
//
// Soundness: removing the ACCESSED directory entry causes the SC dep for that
// key to fail → cache miss.
TEST_F(EvalTraceProperty_ReadDir, RemoveAccessedEntry_Invalidates)
{
    TempDir dir;
    dir.addFile("target", "content");

    auto const expr =
        "(builtins.mapAttrs (name: type: \"${name}:${type}\") "
        "(builtins.readDir " + dir.path().string() + ")).\"target\"";

    // Cold eval: dir has {"target"}.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "target:regular");
    }

    // Warm eval — confirm baseline cache hit.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before removal";
    }

    // Remove "target" — the accessed entry disappears from the listing.
    dir.removeEntry("target");
    invalidateFileCache(dir.path());

    // Must re-evaluate: SC dep for "target" fails (key no longer present).
    // forceRoot will throw because the expression accesses a removed entry.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        EXPECT_THROW(forceRoot(*cache), nix::Error);
        EXPECT_EQ(calls, 1) << "cache miss expected: accessed entry 'target' removed";
    }
}

// ── Test 2: FileContentChange_NoDirChange_StillHits (deterministic) ───────
//
// readDir only hashes names and entry types, not file content.
// Changing the content of an existing file does not change the directory
// listing → the DirectoryEntries dep hash is unchanged → cache hit.
TEST_F(EvalTraceProperty_ReadDir, FileContentChange_NoDirChange_StillHits)
{
    TempDir dir;
    dir.addFile("a", "initial content");

    auto const expr =
        "(builtins.mapAttrs (name: type: \"${name}:${type}\") "
        "(builtins.readDir " + dir.path().string() + ")).\"a\"";

    // Cold eval.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "a:regular");
    }

    // Warm eval — confirm baseline cache hit.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before content change";
    }

    // Overwrite the content of "a" — no add/remove/rename, no type change.
    // readDir does not hash file content; the directory listing is unchanged.
    {
        std::ofstream ofs(dir.path() / "a");
        ofs << "completely different content that is much longer than before";
    }
    invalidateFileCache(dir.path());

    // Must still be a cache hit: the listing (names + types) is identical.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0)
            << "cache hit expected: only file content changed; "
               "readDir dep does not track content, only names and entry types";
    }
}

// ── Test 3: RemoveEntry_Invalidates (deterministic) ───────────────────────
//
// Soundness: removing the ACCESSED entry "a" changes the directory listing
// and causes the SC dep for "a" to fail → cache miss.
TEST_F(EvalTraceProperty_ReadDir, RemoveEntry_Invalidates)
{
    TempDir dir;
    dir.addFile("a", "content a");
    dir.addFile("b", "content b");

    auto const expr =
        "(builtins.mapAttrs (name: type: \"${name}:${type}\") "
        "(builtins.readDir " + dir.path().string() + ")).\"a\"";

    // Cold eval: dir has {"a","b"}.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "a:regular");
    }

    // Warm eval — confirm baseline cache hit.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before removal";
    }

    // Remove "a" — the accessed entry disappears; SC dep for "a" fails.
    dir.removeEntry("a");
    invalidateFileCache(dir.path());

    // Must re-evaluate: SC dep for accessed key "a" fails (key removed).
    // forceRoot will throw because the expression accesses the removed entry.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        EXPECT_THROW(forceRoot(*cache), nix::Error);
        EXPECT_EQ(calls, 1) << "cache miss expected: accessed entry 'a' removed";
    }
}

} // namespace nix::eval_trace::proptest
