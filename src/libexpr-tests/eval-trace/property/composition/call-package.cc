// callPackage with Formals property test.
//
// Expression shape:
//   let f = import <pkg.nix>; in f { data = builtins.readFile <data.txt>; }
//
// pkg.nix content (fixed):
//   { data, prefix ? "pkg" }: "${prefix}-${data}"
//
// This models the nixpkgs callPackage pattern: a function with formal
// arguments (including a default-valued optional formal) is imported from a
// .nix file and called with an attrset whose values come from other files.
//
// Dep slots:
//   slot[0]: Kind::File — data.txt (variable, printable ASCII)
//   slot[1]: Kind::File — pkg.nix  (fixed function body)
//
// Four tests:
//   1. DataFile_Mutation_Invalidates (RapidCheck, maxSuccess=100):
//      Mutate slot[0] → invalidate → assert calls == 1.
//   2. CrossSession_DataFile_WarmHit (deterministic):
//      Cold eval, simulateWarmRestart(), warm eval → assert calls == 0.
//   3. PkgNixFile_BodyChange_Invalidates (deterministic):
//      Change pkg.nix body "${prefix}-${data}" → "${data}-${prefix}"
//      → invalidate → assert calls == 1.
//   4. DefaultArg_NotOverridden_ValueCorrect (deterministic):
//      data.txt = "hello", result must be "pkg-hello" (prefix defaults to "pkg").

#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// ── Fixture ───────────────────────────────────────────────────────────────

class EvalTraceProperty_CallPackage : public TraceCacheFixture {
public:
    EvalTraceProperty_CallPackage() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-call-package");
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
// For every generated callPackage expression:
//   cold eval → warm hit (pre-condition) → mutate slot[0] (data.txt)
//   → invalidate → warm eval must miss (calls == 1).

TEST_F(EvalTraceProperty_CallPackage, DataFile_Mutation_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto expr = *makeCallPackageGen();
            RC_PRE(expr.expectsSuccess());

            auto & slot = expr.depSlots[0];  // data.txt

            // Cold eval — records trace.
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // Warm eval — confirm cache hit (precision pre-condition).
            {
                int calls = 0;
                auto cache = makeCache(expr.nixCode, &calls);
                (void) forceRoot(*cache);
                RC_ASSERT(calls == 0);
            }

            // Mutate data.txt to new printable ASCII content.
            auto mutGen = slot.generateMutation();
            auto newValue = *mutGen;
            RC_PRE(newValue != slot.currentValue);
            slot.mutate(newValue);
            invalidateFileCache(slot.path);

            // Warm eval — must miss because the file content changed.
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

// ── Test 2: CrossSession_DataFile_WarmHit (deterministic) ─────────────────
//
// Cold eval, then simulateWarmRestart() (flushes DB, clears all caches),
// then warm eval → must hit (calls == 0).

TEST_F(EvalTraceProperty_CallPackage, CrossSession_DataFile_WarmHit)
{
    TempTextFile dataFile("hello");
    TempExtFile pkgFile("nix", R"({ data, prefix ? "pkg" }: "${prefix}-${data}")");

    std::string nixCode =
        "let f = import " + pkgFile.path.string() + "; "
        "in f { data = builtins.readFile " + dataFile.path.string() + "; }";

    // Cold eval — records trace.
    {
        auto cache = makeCache(nixCode);
        (void) forceRoot(*cache);
    }

    // Simulate a new evaluation session (flush SQLite, clear caches).
    simulateWarmRestart();

    // Warm eval — must hit from the persisted trace.
    int calls = 0;
    {
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
    }
    EXPECT_EQ(calls, 0);
}

// ── Test 3: PkgNixFile_BodyChange_Invalidates (deterministic) ────────────
//
// Change the pkg.nix body from "${prefix}-${data}" to "${data}-${prefix}"
// (swapping the interpolation order), invalidate, warm eval → calls == 1.

TEST_F(EvalTraceProperty_CallPackage, PkgNixFile_BodyChange_Invalidates)
{
    TempTextFile dataFile("world");
    TempExtFile pkgFile("nix", R"({ data, prefix ? "pkg" }: "${prefix}-${data}")");

    std::string nixCode =
        "let f = import " + pkgFile.path.string() + "; "
        "in f { data = builtins.readFile " + dataFile.path.string() + "; }";

    // Cold eval — records trace.
    {
        auto cache = makeCache(nixCode);
        (void) forceRoot(*cache);
    }

    // Warm eval — confirm baseline hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0);
    }

    // Change pkg.nix body: reverse the interpolation order.
    pkgFile.modify(R"({ data, prefix ? "pkg" }: "${data}-${prefix}")");
    invalidateFileCache(pkgFile.path);

    // Warm eval — must miss because the function body changed.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 1);
    }
}

// ── Test 4: DefaultArg_NotOverridden_ValueCorrect (deterministic) ─────────
//
// data.txt contains "hello"; `prefix` is not supplied so its default "pkg"
// is used.  The result must be the string "pkg-hello".

TEST_F(EvalTraceProperty_CallPackage, DefaultArg_NotOverridden_ValueCorrect)
{
    TempTextFile dataFile("hello");
    TempExtFile pkgFile("nix", R"({ data, prefix ? "pkg" }: "${prefix}-${data}")");

    std::string nixCode =
        "let f = import " + pkgFile.path.string() + "; "
        "in f { data = builtins.readFile " + dataFile.path.string() + "; }";

    // Cold eval — records trace and returns the value.
    auto cache = makeCache(nixCode);
    Value result = forceRoot(*cache);

    ASSERT_EQ(result.type(), nString);
    EXPECT_EQ(std::string(result.string_view()), "pkg-hello");
}

} // namespace nix::eval_trace::proptest
