// Import Tree property test.
//
// Expression shape:
//   import <entry.nix>
//
// where entry.nix contains:
//   let data = builtins.readFile <data.txt>;
//       lib  = import <lib.nix>;
//   in lib data
//
// and lib.nix contains: s: "processed: ${s}"
//
// This models nixpkgs' tree-shaped import pattern.  A 3-file DAG:
//   entry.nix → data.txt   (reads via builtins.readFile)
//   entry.nix → lib.nix    (imports via import)
//   root      → entry.nix  (imports via import)
//
// All three files produce FileBytes deps.  The test verifies that a content
// change in data.txt (the leaf) correctly propagates invalidation up through
// the import chain.
//
// Three tests:
//   1. DataFile_Mutation_Invalidates (RapidCheck): mutate data.txt → miss.
//   2. EntryNixFile_CommentChange_Invalidates (deterministic): append a comment
//      to entry.nix → miss.
//   3. CrossSession_DataFile_WarmHit (deterministic): cold eval, simulateWarmRestart,
//      warm eval → cache hit.

#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <fstream>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// ── Fixture ───────────────────────────────────────────────────────────────

class EvalTraceProperty_ImportTree : public TraceCacheFixture {
public:
    EvalTraceProperty_ImportTree() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-import-tree");
    }
};

static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 100;
    return params;
}

// ── Test 1: DataFile_Mutation_Invalidates (RapidCheck) ────────────────────
//
// Cold eval → warm hit → mutate data.txt → invalidateFileCache → warm eval
// must miss (loaderCalls == 1).
//
// Guards:
//   RC_PRE(expr.expectsSuccess())      — discard error-path inputs
//   RC_PRE(!state.settings.pureEval)   — import requires impure mode
//   (size == 3 and slot[0].kind == File are guaranteed by makeImportTreeGen)

TEST_F(EvalTraceProperty_ImportTree, DataFile_Mutation_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto expr = *makeImportTreeGen();
            RC_PRE(expr.expectsSuccess());
            RC_PRE(!state.settings.pureEval);

            auto & slot = expr.depSlots[0];  // data.txt

            // Cold eval — records trace.
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

            // Mutate data.txt (random printable ASCII; kind==File).
            auto mutGen = slot.generateMutation();
            auto newValue = *mutGen;
            RC_PRE(newValue != slot.currentValue);
            slot.mutate(newValue);
            invalidateFileCache(slot.path);

            // Warm eval — must miss: data.txt FileBytes dep changed.
            {
                int calls = 0;
                auto cache = makeCache(expr.nixCode, &calls);
                (void) forceRoot(*cache);
                RC_ASSERT(calls == 1);
            }

            // Restore for the next iteration.
            slot.restore();
            invalidateFileCache(slot.path);
        },
        makeParams);
}

// ── Test 2: EntryNixFile_CommentChange_Invalidates (deterministic) ────────
//
// Create 3 files manually.
// Cold eval → warm hit → append "# comment\n" to entry.nix →
// invalidateFileCache → warm eval must miss.
//
// Appending a comment changes the FileBytes dep on entry.nix, which must
// propagate invalidation to the root expression.

TEST_F(EvalTraceProperty_ImportTree, EntryNixFile_CommentChange_Invalidates)
{
    // Create the three files manually (deterministic content).
    TempExtFile dataFile("txt",  "hello world");
    TempExtFile libFile("nix",   R"(s: "processed: ${s}")");

    std::string entryContent =
        "let data = builtins.readFile " + dataFile.path.string() + ";"
        " lib = import " + libFile.path.string() + ";"
        " in lib data";
    TempExtFile entryFile("nix", entryContent);

    std::string nixCode = "import " + entryFile.path.string();

    // Cold eval — records trace.
    {
        auto cache = makeCache(nixCode);
        (void) forceRoot(*cache);
    }

    // Warm eval — confirm cache hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0);
    }

    // Append a comment to entry.nix.  This changes its content (FileBytes dep)
    // without altering its evaluation result, but the cache must invalidate
    // because the FileBytes hash changes.
    {
        std::ofstream ofs(entryFile.path, std::ios::app);
        ofs << "\n# comment\n";
    }
    invalidateFileCache(entryFile.path);

    // Warm eval — must miss because entry.nix content changed.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 1);
    }
}

// ── Test 3: CrossSession_DataFile_WarmHit (deterministic) ────────────────
//
// Cold eval → simulateWarmRestart() → warm eval → must hit (loaderCalls == 0).
//
// Verifies that a cross-session warm-hit works for an import tree: the
// FileBytes deps from all three files are stored in the DB and survive
// simulateWarmRestart() (which clears in-memory caches but keeps the DB).

TEST_F(EvalTraceProperty_ImportTree, CrossSession_DataFile_WarmHit)
{
    // Create the three files manually (deterministic content).
    TempExtFile dataFile("txt",  "cross session data");
    TempExtFile libFile("nix",   R"(s: "processed: ${s}")");

    std::string entryContent =
        "let data = builtins.readFile " + dataFile.path.string() + ";"
        " lib = import " + libFile.path.string() + ";"
        " in lib data";
    TempExtFile entryFile("nix", entryContent);

    std::string nixCode = "import " + entryFile.path.string();

    // Cold eval — records trace.
    {
        auto cache = makeCache(nixCode);
        (void) forceRoot(*cache);
    }

    // Simulate a new session: flush DB, clear in-memory caches.
    simulateWarmRestart();

    // Warm eval — must hit from the persisted trace.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0);
    }
}

} // namespace nix::eval_trace::proptest
